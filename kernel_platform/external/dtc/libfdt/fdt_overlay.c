// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * libfdt - Flat Device Tree manipulation
 * Copyright (C) 2016 Free Electrons
 * Copyright (C) 2016 NextThing Co.
 */
#include "libfdt_env.h"

#include <fdt.h>
#include <libfdt.h>
#include <stdio.h>

#include "libfdt_internal.h"

//#define DEBUG
//#define VERBOSE
//#define VERY_VERBOSE

#ifdef DEBUG
#define dprintf(x...)   printf("DEBUG: " x)
#else
#define dprintf(x...)
#endif

#ifdef VERBOSE
#define vdprintf(x...)   printf("VERBOSE: " x)
#else
#define vdprintf(x...)
#endif

#ifdef VERY_VERBOSE
#define vvdprintf(x...)   printf("VERY_VERBOSE: " x)
#else
#define vvdprintf(x...)
#endif

#define MAX_BUF_SIZE	256
#define MAX_ULONG	((unsigned long)~0UL)

static bool find_node_str(const char *path_str, const char *node);

/**
 * overlay_get_target_phandle - retrieves the target phandle of a fragment
 * @fdto: pointer to the device tree overlay blob
 * @fragment: node offset of the fragment in the overlay
 *
 * overlay_get_target_phandle() retrieves the target phandle of an
 * overlay fragment when that fragment uses a phandle (target
 * property) instead of a path (target-path property).
 *
 * returns:
 *      the phandle pointed by the target property
 *      0, if the phandle was not found
 *	-1, if the phandle was malformed
 */
static uint32_t overlay_get_target_phandle(const void *fdto, int fragment)
{
	const fdt32_t *val;
	int len;

	val = fdt_getprop(fdto, fragment, "target", &len);
	if (!val)
		return 0;

	if ((len != sizeof(*val)) || (fdt32_to_cpu(*val) == (uint32_t)-1))
		return (uint32_t)-1;

	return fdt32_to_cpu(*val);
}

/**
 * overlay_get_target - retrieves the offset of a fragment's target
 * @fdt: Base device tree blob
 * @fdto: Device tree overlay blob
 * @fragment: node offset of the fragment in the overlay
 * @pathp: pointer which receives the path of the target (or NULL)
 *
 * overlay_get_target() retrieves the target offset in the base
 * device tree of a fragment, no matter how the actual targeting is
 * done (through a phandle or a path)
 *
 * returns:
 *      the targeted node offset in the base device tree
 *      Negative error code on error
 */
static int overlay_get_target(const void *fdt, const void *fdto,
			      int fragment, char const **pathp)
{
	uint32_t phandle;
	const char *path = NULL;
	int path_len = 0, ret;

	/* Try first to do a phandle based lookup */
	phandle = overlay_get_target_phandle(fdto, fragment);
	if (phandle == (uint32_t)-1)
		return -FDT_ERR_BADPHANDLE;

	/* no phandle, try path */
	if (!phandle) {
		/* And then a path based lookup */
		path = fdt_getprop(fdto, fragment, "target-path", &path_len);
		if (path)
			ret = fdt_path_offset(fdt, path);
		else
			ret = path_len;
	} else
		ret = fdt_node_offset_by_phandle(fdt, phandle);

	/*
	* If we haven't found either a target or a
	* target-path property in a node that contains a
	* __overlay__ subnode (we wouldn't be called
	* otherwise), consider it a improperly written
	* overlay
	*/
	if (ret < 0 && path_len == -FDT_ERR_NOTFOUND)
		ret = -FDT_ERR_BADOVERLAY;

	/* return on error */
	if (ret < 0)
		return ret;

	/* return pointer to path (if available) */
	if (pathp)
		*pathp = path ? path : NULL;

	return ret;
}

/**
 * overlay_phandle_add_offset - Increases a phandle by an offset
 * @fdt: Base device tree blob
 * @node: Device tree overlay blob
 * @name: Name of the property to modify (phandle or linux,phandle)
 * @delta: offset to apply
 *
 * overlay_phandle_add_offset() increments a node phandle by a given
 * offset.
 *
 * returns:
 *      0 on success.
 *      Negative error code on error
 */
static int overlay_phandle_add_offset(void *fdt, int node,
				      const char *name, uint32_t delta)
{
	const fdt32_t *val;
	uint32_t adj_val;
	int len;

	val = fdt_getprop(fdt, node, name, &len);
	if (!val)
		return len;

	if (len != sizeof(*val))
		return -FDT_ERR_BADPHANDLE;

	adj_val = fdt32_to_cpu(*val);
	if ((adj_val + delta) < adj_val)
		return -FDT_ERR_NOPHANDLES;

	adj_val += delta;
	if (adj_val == (uint32_t)-1)
		return -FDT_ERR_NOPHANDLES;

	return fdt_setprop_inplace_u32(fdt, node, name, adj_val);
}

/**
 * overlay_adjust_node_phandles - Offsets the phandles of a node
 * @fdto: Device tree overlay blob
 * @node: Offset of the node we want to adjust
 * @delta: Offset to shift the phandles of
 *
 * overlay_adjust_node_phandles() adds a constant to all the phandles
 * of a given node. This is mainly use as part of the overlay
 * application process, when we want to update all the overlay
 * phandles to not conflict with the overlays of the base device tree.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_adjust_node_phandles(void *fdto, int node,
					uint32_t delta)
{
	int child;
	int ret;

	ret = overlay_phandle_add_offset(fdto, node, "phandle", delta);
	if (ret && ret != -FDT_ERR_NOTFOUND)
		return ret;

	ret = overlay_phandle_add_offset(fdto, node, "linux,phandle", delta);
	if (ret && ret != -FDT_ERR_NOTFOUND)
		return ret;

	fdt_for_each_subnode(child, fdto, node) {
		ret = overlay_adjust_node_phandles(fdto, child, delta);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * overlay_adjust_local_phandles - Adjust the phandles of a whole overlay
 * @fdto: Device tree overlay blob
 * @delta: Offset to shift the phandles of
 *
 * overlay_adjust_local_phandles() adds a constant to all the
 * phandles of an overlay. This is mainly use as part of the overlay
 * application process, when we want to update all the overlay
 * phandles to not conflict with the overlays of the base device tree.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_adjust_local_phandles(void *fdto, uint32_t delta)
{
	/*
	 * Start adjusting the phandles from the overlay root
	 */
	return overlay_adjust_node_phandles(fdto, 0, delta);
}

/**
 * overlay_update_local_node_references - Adjust the overlay references
 * @fdto: Device tree overlay blob
 * @tree_node: Node offset of the node to operate on
 * @fixup_node: Node offset of the matching local fixups node
 * @delta: Offset to shift the phandles of
 *
 * overlay_update_local_nodes_references() update the phandles
 * pointing to a node within the device tree overlay by adding a
 * constant delta.
 *
 * This is mainly used as part of a device tree application process,
 * where you want the device tree overlays phandles to not conflict
 * with the ones from the base device tree before merging them.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_update_local_node_references(void *fdto,
						int tree_node,
						int fixup_node,
						uint32_t delta)
{
	int fixup_prop;
	int fixup_child;
	int ret;

	fdt_for_each_property_offset(fixup_prop, fdto, fixup_node) {
		const fdt32_t *fixup_val;
		const char *tree_val;
		const char *name;
		int fixup_len;
		int tree_len;
		int i;

		fixup_val = fdt_getprop_by_offset(fdto, fixup_prop,
						  &name, &fixup_len);
		vdprintf
		    ("%s: %d: +fdto-getprop_by_offset(prop=%d from node=%s) fixup_len %d\n",
		     __func__, __LINE__, fixup_prop, fdt_get_name(fdto, fixup_node, NULL),
		     fixup_len);
		if (!fixup_val)
			return fixup_len;

		if (fixup_len % sizeof(uint32_t))
			return -FDT_ERR_BADOVERLAY;

		tree_val = fdt_getprop(fdto, tree_node, name, &tree_len);
		vdprintf
		    ("%s: %d: +fdto-getprop(name=%s from node=%s) tree_val(%s), tree_len(%d)\n",
		     __func__, __LINE__, name, fdt_get_name(fdto, fixup_node, NULL), tree_val,
		     tree_len);
		if (!tree_val) {
			if (tree_len == -FDT_ERR_NOTFOUND)
				return -FDT_ERR_BADOVERLAY;

			return tree_len;
		}

		for (i = 0; i < (fixup_len / sizeof(uint32_t)); i++) {
			fdt32_t adj_val;
			uint32_t poffset;

			poffset = fdt32_to_cpu(fixup_val[i]);

			/*
			 * phandles to fixup can be unaligned.
			 *
			 * Use a memcpy for the architectures that do
			 * not support unaligned accesses.
			 */
			memcpy(&adj_val, tree_val + poffset, sizeof(adj_val));

			adj_val = cpu_to_fdt32(fdt32_to_cpu(adj_val) + delta);

			ret = fdt_setprop_inplace_namelen_partial(fdto,
								  tree_node,
								  name,
								  strlen(name),
								  poffset,
								  &adj_val,
								  sizeof(adj_val));
			vdprintf
			    ("%s: %d: +fdto-setprop_inplace_namelen_partial, name %s, "
			     "off %u, val %u, ret %d\n", __func__, __LINE__,
			     name, poffset, fdt32_to_cpu(adj_val), ret);
			if (ret == -FDT_ERR_NOSPACE)
				return -FDT_ERR_BADOVERLAY;

			if (ret)
				return ret;
		}
	}

	fdt_for_each_subnode(fixup_child, fdto, fixup_node) {
		const char *fixup_child_name = fdt_get_name(fdto, fixup_child,
							    NULL);
		int tree_child;

		tree_child = fdt_subnode_offset(fdto, tree_node,
						fixup_child_name);
		vdprintf
		    ("%s: %d: +fdto-subnode_offset(fixup_child=%s from node=%s), ret %d\n",
		     __func__, __LINE__, fixup_child_name,
		     fdt_get_name(fdto, tree_node, NULL), tree_child);
		if (tree_child == -FDT_ERR_NOTFOUND)
			return -FDT_ERR_BADOVERLAY;
		if (tree_child < 0)
			return tree_child;

		ret = overlay_update_local_node_references(fdto,
							   tree_child,
							   fixup_child,
							   delta);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * overlay_update_local_references - Adjust the overlay references
 * @fdto: Device tree overlay blob
 * @delta: Offset to shift the phandles of
 *
 * overlay_update_local_references() update all the phandles pointing
 * to a node within the device tree overlay by adding a constant
 * delta to not conflict with the base overlay.
 *
 * This is mainly used as part of a device tree application process,
 * where you want the device tree overlays phandles to not conflict
 * with the ones from the base device tree before merging them.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_update_local_references(void *fdto, uint32_t delta)
{
	int fixups;

	fixups = fdt_path_offset(fdto, "/__local_fixups__");
	dprintf("%s: +fdto_path_offset for /__local_fixups__ %d\n", __func__,
		fixups);
	if (fixups < 0) {
		/* There's no local phandles to adjust, bail out */
		if (fixups == -FDT_ERR_NOTFOUND)
			return 0;

		return fixups;
	}

	/*
	 * Update our local references from the root of the tree
	 */
	return overlay_update_local_node_references(fdto, 0, fixups,
						    delta);
}

/**
 * overlay_fixup_one_phandle - Set an overlay phandle to the base one
 * @fdt: Base Device Tree blob
 * @fdto: Device tree overlay blob
 * @symbols_off: Node offset of the symbols node in the base device tree
 * @path: Path to a node holding a phandle in the overlay
 * @path_len: number of path characters to consider
 * @name: Name of the property holding the phandle reference in the overlay
 * @name_len: number of name characters to consider
 * @poffset: Offset within the overlay property where the phandle is stored
 * @label: Label of the node referenced by the phandle
 *
 * overlay_fixup_one_phandle() resolves an overlay phandle pointing to
 * a node in the base device tree.
 *
 * This is part of the device tree overlay application process, when
 * you want all the phandles in the overlay to point to the actual
 * base dt nodes.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_fixup_one_phandle(void *fdt, void *fdto,
				     int symbols_off,
				     const char *path, uint32_t path_len,
				     const char *name, uint32_t name_len,
				     int poffset, const char *label)
{
	const char *symbol_path;
	uint32_t phandle;
	fdt32_t phandle_prop;
	int symbol_off, fixup_off;
	int prop_len;

	if (symbols_off < 0)
		return symbols_off;

	symbol_path = fdt_getprop(fdt, symbols_off, label,
				  &prop_len);
	vvdprintf("%s: %d: +fdt - path(%s), name(%s), label(%s) sym_path(%s)\n",
		  __func__, __LINE__, path, name, label, symbol_path);
	if (!symbol_path)
		return prop_len;

	symbol_off = fdt_path_offset(fdt, symbol_path);
	vvdprintf
	    ("%s: %d: +fdt - path(%s), name(%s), sym_path(%s), symbol_off(%d)\n",
	     __func__, __LINE__, path, name, symbol_path, symbol_off);
	if (symbol_off < 0)
		return symbol_off;

	phandle = fdt_get_phandle(fdt, symbol_off);
	vvdprintf
	    ("%s: %d: +fdt - path(%s), name(%s), sym_path(%s), phandle(%u)\n",
	     __func__, __LINE__, path, name, symbol_path, phandle);
	if (!phandle)
		return -FDT_ERR_NOTFOUND;

	fixup_off = fdt_path_offset_namelen(fdto, path, path_len);
	vvdprintf
	    ("%s: %d: +fdto - path(%s), name(%s), sym_path(%s), fixup_off(%u)\n",
	     __func__, __LINE__, path, name, symbol_path, fixup_off);
	if (fixup_off == -FDT_ERR_NOTFOUND)
		return -FDT_ERR_BADOVERLAY;
	if (fixup_off < 0)
		return fixup_off;

	phandle_prop = cpu_to_fdt32(phandle);
	vvdprintf
	    ("%s: %d: +ftdo - setprop: path(%s), name(%s), sym_path(%s), phandle_prop(%u)\n",
	     __func__, __LINE__, path, name, symbol_path, phandle_prop);
	return fdt_setprop_inplace_namelen_partial(fdto, fixup_off,
						   name, name_len, poffset,
						   &phandle_prop,
						   sizeof(phandle_prop));
};

/**
 * overlay_fixup_phandle - Set an overlay phandle to the base one
 * @fdt: Base Device Tree blob
 * @fdto: Device tree overlay blob
 * @symbols_off: Node offset of the symbols node in the base device tree
 * @property: Property offset in the overlay holding the list of fixups
 * @fixups_off: Offset of __fixups__ node in @fdto
 *
 * overlay_fixup_phandle() resolves all the overlay phandles pointed
 * to in a __fixups__ property, and updates them to match the phandles
 * in use in the base device tree.
 *
 * This is part of the device tree overlay application process, when
 * you want all the phandles in the overlay to point to the actual
 * base dt nodes.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_fixup_phandle(void *fdt, void *fdto, int symbols_off,
				 int property, int fixups_off)
{
	const char *value;
	const char *label;
	int len, ret = 0;

	value = fdt_getprop_by_offset(fdto, property, &label, &len);
	if (!value) {
		if (len == -FDT_ERR_NOTFOUND)
			return -FDT_ERR_INTERNAL;

		return len;
	}

	do {
		const char *path, *name, *fixup_end;
		const char *fixup_str = value;
		uint32_t path_len, name_len;
		uint32_t fixup_len;
		char *sep, *endptr;
		int poffset;

		fixup_end = memchr(value, '\0', len);
		if (!fixup_end)
			return -FDT_ERR_BADOVERLAY;
		fixup_len = fixup_end - fixup_str;

		len -= fixup_len + 1;
		value += fixup_len + 1;

		path = fixup_str;
		sep = memchr(fixup_str, ':', fixup_len);
		if (!sep || *sep != ':')
			return -FDT_ERR_BADOVERLAY;

		path_len = sep - path;
		if (path_len == (fixup_len - 1))
			return -FDT_ERR_BADOVERLAY;

		fixup_len -= path_len + 1;
		name = sep + 1;
		sep = memchr(name, ':', fixup_len);
		if (!sep || *sep != ':')
			return -FDT_ERR_BADOVERLAY;

		name_len = sep - name;
		if (!name_len)
			return -FDT_ERR_BADOVERLAY;

		poffset = strtoul(sep + 1, &endptr, 10);
		if ((*endptr != '\0') || (endptr <= (sep + 1)))
			return -FDT_ERR_BADOVERLAY;

		ret = overlay_fixup_one_phandle(fdt, fdto, symbols_off,
						path, path_len, name, name_len,
						poffset, label);
		if (ret)
			return ret;
	} while (len > 0);

	return ret;
}

/**
 * overlay_fixup_phandles - Resolve the overlay phandles to the base
 *                          device tree
 * @fdt: Base Device Tree blob
 * @fdto: Device tree overlay blob
 * @merge_olay_max_phdl: Pointer to max phandle value for merged blobs,
 *     Both input blobs are overlay blobs that are being merged
 *
 * overlay_fixup_phandles() resolves all the overlay phandles pointing
 * to nodes in the base device tree.
 *
 * This is one of the steps of the device tree overlay application
 * process, when you want all the phandles in the overlay to point to
 * the actual base dt nodes.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_fixup_phandles(void *fdt, void *fdto,
				  uint32_t merge_olay_max_phdl)
{
	int fixups_off, symbols_off;
	int property, ret = 0;

	/* We can have overlays without any fixups */
	fixups_off = fdt_path_offset(fdto, "/__fixups__");
	dprintf("%s: +fdto_path_offset for /__fixups__ %d\n", __func__,
		fixups_off);
	if (fixups_off == -FDT_ERR_NOTFOUND)
		return 0;	/* nothing to do */
	if (fixups_off < 0)
		return fixups_off;

	/* And base DTs without symbols */
	symbols_off = fdt_path_offset(fdt, "/__symbols__");
	dprintf("%s: +fdt_path_offset for /__symbols__ %d\n", __func__,
		symbols_off);
	if ((symbols_off < 0 && (symbols_off != -FDT_ERR_NOTFOUND)))
		return symbols_off;

	fdt_for_each_property_offset(property, fdto, fixups_off) {
		ret = overlay_fixup_phandle(fdt, fdto, symbols_off,
					    property, fixups_off);
		if (ret && (!merge_olay_max_phdl || ret != -FDT_ERR_NOTFOUND))
			return ret;
	}

	return ret;
}

/**
 * overlay_apply_node - Merges a node into the base device tree
 * @fdt: Base Device Tree blob
 * @target: Node offset in the base device tree to apply the fragment to
 * @fdto: Device tree overlay blob
 * @node: Node offset in the overlay holding the changes to merge
 *
 * overlay_apply_node() merges a node into a target base device tree
 * node pointed.
 *
 * This is part of the final step in the device tree overlay
 * application process, when all the phandles have been adjusted and
 * resolved and you just have to merge overlay into the base device
 * tree.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_apply_node(void *fdt, int target,
			      void *fdto, int node)
{
	int property;
	int subnode;

	fdt_for_each_property_offset(property, fdto, node) {
		const char *name;
		const void *prop;
		int prop_len;
		int ret;

		prop = fdt_getprop_by_offset(fdto, property, &name,
					     &prop_len);
		if (prop_len == -FDT_ERR_NOTFOUND)
			return -FDT_ERR_INTERNAL;
		if (prop_len < 0)
			return prop_len;

		ret = fdt_setprop(fdt, target, name, prop, prop_len);
		if (ret)
			return ret;
	}

	fdt_for_each_subnode(subnode, fdto, node) {
		const char *name = fdt_get_name(fdto, subnode, NULL);
		int nnode;
		int ret;

		nnode = fdt_add_subnode(fdt, target, name);
		if (nnode == -FDT_ERR_EXISTS) {
			nnode = fdt_subnode_offset(fdt, target, name);
			if (nnode == -FDT_ERR_NOTFOUND) {
				dprintf
				    ("%s: %d: +subnode_offset failed: fdto-subnode(%s) "
				     "to fdt target %d, nnode %d\n",
				     __func__, __LINE__, name, target, nnode);
				return -FDT_ERR_INTERNAL;
			}
		}

		if (nnode < 0) {
			dprintf
			    ("%s: %d: ++subnode add/offset failed: fdto-subnode(%s) "
			     "to fdt target %d, nnode %d\n",
			     __func__, __LINE__, name, target, nnode);
			return nnode;
		}
		vdprintf
		    ("%s: %d: ++subnode add/offset fdto-subnode(%s) to fdt "
		     "target %d, nnode %d\n",
		     __func__, __LINE__, name, target, nnode);

		ret = overlay_apply_node(fdt, nnode, fdto, subnode);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * copy_node - copy a node hierarchically
 * @fdt - pointer to base device tree
 * @fdto - pointer to overlay device tree
 * @fdto_child - offset of node in overlay device tree which needs to be copied
 * @fdt_parent - offset of parent node in base tree under which @fdto_child
 *		need to be copied
 * @name - if not NULL, (new) name of the child in base device tree
 * @skip_fdto_child - if set, skips creation of @fdto_child under @fdt_parent.
 *      Instead copies everything under @fdto_child to @fdt_parent.
 *
 * This function copies a node in overlay tree along with its child-nodes and
 * their properties, under a given parent node in base tree.
 */
static int copy_node(void *fdt, void *fdto, int fdt_parent,
		     int fdto_child, const char *name, int skip_fdto_child)
{
	int len, prop, parent, child;

	if (!skip_fdto_child) {
		if (!name) {
			name = fdt_get_name(fdto, fdto_child, &len);
			if (!name)
				return len;
		}

		parent = fdt_subnode_offset(fdt, fdt_parent, name);
		if (parent < 0) {
			parent = fdt_add_subnode(fdt, fdt_parent, name);
		}

		vdprintf
		    ("%s: No skip: In fdt-parent-node(%s) add fdto-child(%s), parent %d\n",
		     __func__, fdt_get_name(fdt, fdt_parent, NULL), name, parent);
		if (parent < 0)
			return parent;
	} else {
		parent = fdt_parent;
		vdprintf
		    ("%s: Skip: In fdt-parent-node(%s), skip fdto-child(%s), parent %d\n",
		     __func__, fdt_get_name(fdt, fdt_parent, NULL), name, parent);
	}

	fdt_for_each_property_offset(prop, fdto, fdto_child) {
		int ret, fdt_len = 0;
		const char *value, *pname;
		void *p;

		value = fdt_getprop_by_offset(fdto, prop, &pname, &len);
		if (!value)
			return len;

		if (fdt_getprop(fdt, parent, pname, &fdt_len))
			len += fdt_len;

		ret = fdt_setprop_placeholder(fdt, parent, pname, len, &p);
		vvdprintf
		    ("%s: +setprop: In fdt-parent-node(%s), add fdto-prop(%s), "
		     "value(%s), ret %d\n", __func__, fdt_get_name(fdt, parent, NULL),
		     pname, value, ret);
		if (ret)
			return ret;

		if (fdt_len > 0) {
			p = (char *)p + fdt_len;
			len -= fdt_len;
			vvdprintf("%s: fdto-prop(%s): Copy only new values\n",
				  __func__, pname);
		}

		memcpy(p, value, len);
	}

	fdt_for_each_subnode(child, fdto, fdto_child) {
		int ret;

		ret = copy_node(fdt, fdto, parent, child, NULL, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static int get_fragment_name(void *fdto, int fragment, char *name, int namelen)
{
	int len;
	const char *nname;
	int size = sizeof("fragment@") - 1;

	nname = fdt_get_name(fdto, fragment, &len);
	if (!nname)
		return len;

	if (len < size || len >= namelen || memcmp(nname, "fragment@", size))
		return -FDT_ERR_BADVALUE;

	memcpy(name, nname, len);
	name[len] = 0;

	return 0;
}

static int get_fragment_index(char *name, unsigned long *idxp)
{
	char *idx;
	int size = sizeof("fragment@") - 1;
	int len = strlen(name);
	char *stop;
	unsigned long index;

	if (len < size)
		return -FDT_ERR_BADVALUE;

	idx = name + size;
	index = strtoul(idx, &stop, 10);
	if (*stop != '\0' || stop <= idx)
		return -FDT_ERR_BADVALUE;

	*idxp = index;

	return 0;
}

static int set_new_fragment_name(char *name, int namelen,
				 unsigned long base_fragment_count)
{
	unsigned long index;
	int ret;

	ret = get_fragment_index(name, &index);
	if (ret)
		return ret;

	if (MAX_ULONG - base_fragment_count < index)
		return -FDT_ERR_INTERNAL;

	index += base_fragment_count;

	ret = snprintf(name, namelen, "fragment@%lu", index);

	return ret >= namelen ? -FDT_ERR_INTERNAL : 0;
}

static int add_phandle(void *fdt, char *node_name, uint32_t phandle)
{
	int offset;

	offset = fdt_subnode_offset(fdt, 0, node_name);
	if (offset < 0)
		return offset;

	return fdt_setprop_u32(fdt, offset, "phandle", phandle);
}

static int copy_fragment_to_base(void *fdt, void *fdto,
				 int fragment, uint32_t *merge_olay_max_phdl,
				 unsigned long *base_fragment_count)
{
	char name[MAX_BUF_SIZE];
	int ret;
	uint32_t target_phandle = *merge_olay_max_phdl;

	if (merge_olay_max_phdl == NULL)
		return -FDT_ERR_BADPHANDLE;

	ret = get_fragment_name(fdto, fragment, name, sizeof(name));
	if (ret)
		return ret;

	ret = set_new_fragment_name(name, sizeof(name), *base_fragment_count);
	if (ret)
		return ret;

	vdprintf("%s: -copy_node: fdto-fragment(%s) to fdt-fragment(%s)\n",
		 __func__, fdt_get_name(fdto, fragment, NULL), name);
	ret = copy_node(fdt, fdto, 0, fragment, name, 0);
	if (ret)
		return ret;

	ret = add_phandle(fdt, name, target_phandle);
	if (ret)
		return ret;

	// Fix target to point to new node in base
	ret = fdt_setprop_inplace_u32(fdto, fragment, "target", target_phandle);
	if (ret)
		return ret;

	return (++(*merge_olay_max_phdl) == UINT32_MAX ||
		++(*base_fragment_count) == ULONG_MAX) ?
	    -FDT_ERR_BADOVERLAY : 0;
}

static int count_fragments(void *fdt, unsigned long *max_base_fragments);

/**
 * overlay_merge - Merge an overlay into its base device tree
 * @fdt: Base Device Tree blob
 * @fdto: Device tree overlay blob
 * @merge: Both input blobs are overlay blobs that are being merged
 *
 * overlay_merge() merges an overlay into its base device tree.
 *
 * This is the next to last step in the device tree overlay application
 * process, when all the phandles have been adjusted and resolved and
 * you just have to merge overlay into the base device tree.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_merge(void *fdt, void *fdto, uint32_t *merge_olay_max_phdl)
{
	int fragment, ret;
	unsigned long base_fragment_count = 0;

	if (merge_olay_max_phdl) {
		ret = count_fragments(fdt, &base_fragment_count);
		/* no fragments in base dtb? then nothing to rename */
		if (ret && ret != -FDT_ERR_NOTFOUND)
			return ret;

		base_fragment_count++;
	}

	fdt_for_each_subnode(fragment, fdto, 0) {
		int overlay;
		int target;

		/*
		 * Each fragments will have an __overlay__ node. If
		 * they don't, it's not supposed to be merged
		 */
		overlay = fdt_subnode_offset(fdto, fragment, "__overlay__");
		if (overlay == -FDT_ERR_NOTFOUND)
			continue;

		if (overlay < 0)
			return overlay;

		target = overlay_get_target(fdt, fdto, fragment, NULL);
		if (target < 0) {
			if (!merge_olay_max_phdl
			    || target != -FDT_ERR_BADPHANDLE)
				return target;

			/*
			 * No target found which is acceptable situation in case
			 * of merging two overlay blobs. Copy this fragment to
			 * base/combined blob, so that it can be considered for
			 * overlay during a subsequent overlay operation of
			 * combined blob on another base blob.
			 */
			vdprintf
			    ("%s: -copy_fragment_to_base: fdto-fragment(%s)\n",
			     __func__, fdt_get_name(fdto, fragment, NULL));
			ret = copy_fragment_to_base(fdt, fdto, fragment,
						    merge_olay_max_phdl,
						    &base_fragment_count);
			if (ret)
				return ret;

			continue;
		}

		vdprintf
		    ("%s: -overlay_apply_node: fdto-fragment(%s) to fdt-target %d\n",
		     __func__, fdt_get_name(fdto, fragment, NULL), target);
		ret = overlay_apply_node(fdt, target, fdto, overlay);
		if (ret)
			return ret;
	}

	return 0;
}

static int get_path_len(const void *fdt, int nodeoffset)
{
	int len = 0, namelen;
	const char *name;

	FDT_RO_PROBE(fdt);

	for (;;) {
		name = fdt_get_name(fdt, nodeoffset, &namelen);
		if (!name)
			return namelen;

		/* root? we're done */
		if (namelen == 0)
			break;

		nodeoffset = fdt_parent_offset(fdt, nodeoffset);
		if (nodeoffset < 0)
			return nodeoffset;
		len += namelen + 1;
	}

	/* in case of root pretend it's "/" */
	if (len == 0)
		len++;
	return len;
}

/**
 * overlay_symbol_update - Update the symbols of base tree after a merge
 * @fdt: Base Device Tree blob
 * @fdto: Device tree overlay blob
 * @merge_olay_max_phdl: Pointer to max phandle value for merged blobs,
 *         Both input blobs are overlay blobs that are being merged
 *
 * overlay_symbol_update() updates the symbols of the base tree with the
 * symbols of the applied overlay
 *
 * This is the last step in the device tree overlay application
 * process, allowing the reference of overlay symbols by subsequent
 * overlay operations.
 *
 * returns:
 *      0 on success
 *      Negative error code on failure
 */
static int overlay_symbol_update(void *fdt, void *fdto,
				 uint32_t merge_olay_max_phdl)
{
	int root_sym, ov_sym, prop, next_prop, path_len, fragment, target;
	int len, frag_name_len, ret, rel_path_len, rel_path_len_olay = 0;
	const char *s, *e;
	const char *path;
	const char *name;
	const char *frag_name;
	const char *rel_path, *rel_path_olay = NULL;
	const char *target_path;
	char *buf;
	void *p;

	ov_sym = fdt_subnode_offset(fdto, 0, "__symbols__");

	/* if no overlay symbols exist no problem */
	if (ov_sym < 0)
		return 0;

	root_sym = fdt_subnode_offset(fdt, 0, "__symbols__");

	/* it no root symbols exist we should create them */
	if (root_sym == -FDT_ERR_NOTFOUND)
		root_sym = fdt_add_subnode(fdt, 0, "__symbols__");

	/* any error is fatal now */
	if (root_sym < 0)
		return root_sym;

	/* iterate over each overlay symbol */

	/* Safeguard against property being possibly deleted in this loop */
	prop = fdt_first_property_offset(fdto, ov_sym);
	while (prop >= 0) {
		next_prop = fdt_next_property_offset(fdto, prop);

		path = fdt_getprop_by_offset(fdto, prop, &name, &path_len);
		if (!path)
			return path_len;

		/* verify it's a string property (terminated by a single \0) */
		if (path_len < 1
		    || memchr(path, '\0', path_len) != &path[path_len - 1])
			return -FDT_ERR_BADVALUE;

		/* keep end marker to avoid strlen() */
		e = path + path_len;

		if (*path != '/')
			return -FDT_ERR_BADVALUE;

		/* get fragment name first */
		s = strchr(path + 1, '/');
		if (!s) {
			/* Symbol refers to something that won't end
			 * up in the target tree */
			continue;
		}

		frag_name = path + 1;
		frag_name_len = s - path - 1;

		/* verify format; safe since "s" lies in \0 terminated prop */
		len = sizeof("/__overlay__/") - 1;
		if ((e - s) > len && (memcmp(s, "/__overlay__/", len) == 0)) {
			/* /<fragment-name>/__overlay__/<relative-subnode-path> */
			rel_path = s + len;
			rel_path_len = e - rel_path - 1;

			if (merge_olay_max_phdl != 0) {
				rel_path_olay = s + 1;
				rel_path_len_olay = e - rel_path_olay - 1;
			}
		} else if ((e - s) == len
			   && (memcmp(s, "/__overlay__", len - 1) == 0)) {
			/* /<fragment-name>/__overlay__ */
			rel_path = "";
			rel_path_len = 0;
		} else {
			/* Symbol refers to something that won't end
			 * up in the target tree */
			continue;
		}

		/* find the fragment index in which the symbol lies */
		ret = fdt_subnode_offset_namelen(fdto, 0, frag_name,
						 frag_name_len);
		/* not found? */
		if (ret < 0)
			return -FDT_ERR_BADOVERLAY;
		fragment = ret;

		/* an __overlay__ subnode must exist */
		ret = fdt_subnode_offset(fdto, fragment, "__overlay__");
		if (ret < 0)
			return -FDT_ERR_BADOVERLAY;

		/* get the target of the fragment */
		ret = overlay_get_target(fdt, fdto, fragment, &target_path);
		if (ret < 0)
			return ret;

		target = ret;

		/* Before proceeding further, check if you need to update
		 * rel_path for dtbo-dtbo merging case
		 */
		if (rel_path_olay) {
			uint32_t phandle =
			    overlay_get_target_phandle(fdto, fragment);
			int base_symbol_found = (phandle < merge_olay_max_phdl);

			/* For new nodes we need the overlay string in path */
			if (!base_symbol_found) {
				vdprintf
				    ("%s: fdto: name(%s), rel_path(%s) changed "
				     "to rel_path_olay(%s)\n",
				     __func__, name, rel_path, rel_path_olay);
				rel_path = rel_path_olay;
				rel_path_len = rel_path_len_olay;
			}
		}

		/* if we have a target path use */
		if (!target_path) {
			ret = get_path_len(fdt, target);
			if (ret < 0)
				return ret;
			len = ret;
		} else {
			len = strlen(target_path);
		}

		ret = fdt_setprop_placeholder(fdt, root_sym, name,
					      len + (len >
						     1) + rel_path_len + 1, &p);
		vvdprintf
		    ("%s: +fdt-setprop: for fdto-name(%s), rel_path(%s), "
		     "rel_path_olay(%s)\n", __func__, name, rel_path, rel_path_olay);
		if (ret < 0)
			return ret;

		if (!target_path) {
			/* again in case setprop_placeholder changed it */
			ret =
			    overlay_get_target(fdt, fdto, fragment,
					       &target_path);
			if (ret < 0)
				return ret;
			target = ret;
		}

		buf = p;
		if (len > 1) {	/* target is not root */
			if (!target_path) {
				ret = fdt_get_path(fdt, target, buf, len + 1);
				if (ret < 0)
					return ret;
			} else
				memcpy(buf, target_path, len + 1);

		} else {
			len--;
		}

		buf[len] = '/';
		memcpy(buf + len + 1, rel_path, rel_path_len);
		buf[len + 1 + rel_path_len] = '\0';
		vdprintf("%s: fdt-final-path: for name(%s) is (%s)\n", __func__,
			 name, buf);

		prop = next_prop;
	}

	return 0;
}

int fdt_overlay_apply(void *fdt, void *fdto)
{
	uint32_t delta;
	int ret;

	FDT_RO_PROBE(fdt);
	FDT_RO_PROBE(fdto);

	ret = fdt_find_max_phandle(fdt, &delta);
	dprintf("%s: *** +max_phandle %d, ret %d ***\n", __func__, delta, ret);
	if (ret)
		goto err;

	ret = overlay_adjust_local_phandles(fdto, delta);
	dprintf("%s: *** +adjust_local_phandles, ret %d ***\n", __func__, ret);
	if (ret)
		goto err;

	ret = overlay_update_local_references(fdto, delta);
	dprintf("%s: *** +update_local_references, ret %d ***\n", __func__,
		ret);
	if (ret)
		goto err;

	ret = overlay_fixup_phandles(fdt, fdto, 0);
	dprintf("%s: *** +fixup_phandles(0), ret %d ***\n", __func__, ret);
	if (ret)
		goto err;

	ret = overlay_merge(fdt, fdto, NULL);
	dprintf("%s: *** merge(0), ret %d ***\n", __func__, ret);
	if (ret)
		goto err;

	ret = overlay_symbol_update(fdt, fdto, 0);
	dprintf("%s: *** symbol_update(0), ret %d ***\n", __func__, ret);
	if (ret)
		goto err;

	/*
	 * The overlay has been damaged, erase its magic.
	 */
	fdt_set_magic(fdto, ~0);
	dprintf("%s *** completed successfully! ***\n", __func__);

	return 0;

err:
	/*
	 * The overlay might have been damaged, erase its magic.
	 */
	fdt_set_magic(fdto, ~0);

	/*
	 * The base device tree might have been damaged, erase its
	 * magic.
	 */
	fdt_set_magic(fdt, ~0);
	dprintf("%s *** failed with ret %d ***\n", __func__, ret);

	return ret;
}

/* Return maximum count of overlay fragments */
static int count_fragments(void *fdt, unsigned long *max_base_fragments)
{
	int offset = -1, child_offset, child_len, len, found = 0;
	const char *name, *child_name, *idx;
	char *stop;
	unsigned long index, max = 0;

	offset = fdt_first_subnode(fdt, 0);
	while (offset >= 0) {
		name = fdt_get_name(fdt, offset, &len);
		if (!name)
			return len;

		if (len < 9 || memcmp(name, "fragment@", 9))
			goto next_node;

		child_offset = fdt_first_subnode(fdt, offset);
		if (child_offset < 0)
			return child_offset;

		child_name = fdt_get_name(fdt, child_offset, &child_len);
		if (!child_name)
			return child_len;

		if (child_len < 11 || memcmp(child_name, "__overlay__", 11))
			goto next_node;

		found = 1;
		idx = name + 9;
		index = strtoul(idx, &stop, 10);
		if (index > max)
			max = index;
next_node:
		offset = fdt_next_subnode(fdt, offset);
	}

	if (!found)
		return -FDT_ERR_NOTFOUND;

	*max_base_fragments = max;

	return 0;
}

static int find_add_subnode(void *fdt, int parent_off, char *node_name)
{
	int offset;

	offset = fdt_subnode_offset(fdt, parent_off, node_name);

	if (offset < 0)
		offset = fdt_add_subnode(fdt, parent_off, node_name);

	return offset;
}

static int prop_exists_in_node(void *fdt, char *path, const char *prop_name)
{
	int offset;
	const void *val;

	offset = fdt_path_offset(fdt, path);
	if (offset < 0)
		return 0;

	val = fdt_getprop(fdt, offset, prop_name, NULL);

	return val != NULL;
}

static void *get_next_component(const char **p, int *len, char sep)
{
	char *q;
	int consumed;

	q = memchr(*p, sep, *len);
	if (!q)
		return NULL;

	q++;

	// 1 for ':'
	consumed = (q - *p);
	if (*len <= consumed)
		return NULL;

	*len -= consumed;
	*p = q;

	return q;
}

static int lookup_target_path(void *fdt, void *fdto, const char *fragment,
			      int frag_name_len, char *buf, int buf_len,
			      int *target_off, int *root_path)
{
	int offset, ret, target, len;
	const char *target_path;
	static const char fragstr[] = "fragment@";
	int fragstrlen = sizeof(fragstr) - 1;

	memset(buf, 0, buf_len);

	/* Check fdto-fragment has fragment string */
	if (frag_name_len < fragstrlen || memcmp(fragment, fragstr, fragstrlen))
		return -FDT_ERR_BADOVERLAY;

	/* find the fragment index in which the symbol lies */
	ret = fdt_subnode_offset_namelen(fdto, 0, fragment, frag_name_len);
	/* not found? */
	if (ret < 0)
		return -FDT_ERR_BADOVERLAY;

	offset = ret;

	/* an __overlay__ subnode must exist */
	ret = fdt_subnode_offset(fdto, offset, "__overlay__");
	if (ret < 0)
		return -FDT_ERR_BADOVERLAY;

	/* get the target of the fragment */
	ret = overlay_get_target(fdt, fdto, offset, &target_path);
	if (ret < 0)
		return ret;

	target = ret;
	if (target_off)
		*target_off = ret;

	/* if we have a target path use */
	if (!target_path) {
		ret = get_path_len(fdt, target);
		if (ret < 0)
			return ret;
		len = ret;
	} else {
		len = strlen(target_path);
	}
	if (len >= buf_len)
		return -FDT_ERR_INTERNAL;

	if (len > 1) {		/* target is not root */
		if (!target_path) {
			ret = fdt_get_path(fdt, target, buf, len + 1);
			if (ret < 0)
				return ret;
		} else
			memcpy(buf, target_path, len + 1);

	}

	/* Check fdt-path is having fragments or it part of root path */
	if (root_path && find_node_str(buf, fragstr))
		*root_path = 0;
	else
		*root_path = 1;

	return 0;
}

static int fixup_snippet_update(void *fdt, void *fdto, const char *snippet,
				int snippet_len, char *buf, int buflen,
				int *ignore, int base_symbol_found,
				uint32_t merge_olay_max_phdl)
{
	const char *snippet_o = snippet;
	const char *path, *fragment, *prop_name, *prop_val, *rel_path;
	char *sep;
	int snippet_len_o = snippet_len, fragment_len, rel_path_len;
	int prop_len, path_len, rem, ret;
	static const char tprop[] = "target";
	static const char frag[] = "/fragment";
	static const char olay[] = "/__overlay__";
	int root_path = 0;

	/* Validate format:
	 *      path_to_node : prop_name : prop_offset
	 */
	path = snippet;
	prop_name = get_next_component(&snippet, &snippet_len, ':');
	if (!prop_name)
		return -FDT_ERR_BADOVERLAY;

	prop_val = get_next_component(&snippet, &snippet_len, ':');
	if (!prop_val)
		return -FDT_ERR_BADOVERLAY;

	path_len = prop_name - path - 1;	// 1 for ':'
	prop_len = prop_val - prop_name - 1;	// 1 for ':'

	if (path_len < sizeof(frag) - 1 || memcmp(path, frag, sizeof(frag) - 1))
		return -FDT_ERR_BADOVERLAY;

	vdprintf("%s: snippet(%s): fdto-prop(%s), val(%s)\n", __func__,
		 snippet_o, prop_name, prop_val);
	if (base_symbol_found && prop_len == sizeof(tprop) - 1
	    && !memcmp(prop_name, tprop, sizeof(tprop) - 1)) {
		*ignore = 1;
		return 0;
	}

	fragment = path;
	// check if there is a '/' besides the first one in node_path
	sep = memchr(fragment + 1, '/', path_len - 1);
	if (sep) {
		fragment_len = sep - fragment;
		path_len -= (sep - fragment);
		if (path_len < sizeof(olay) - 1
		    || memcmp(sep, olay, sizeof(olay) - 1))
			return -FDT_ERR_BADOVERLAY;

		{
			int frag_offset;

			frag_offset =
			    fdt_subnode_offset_namelen(fdto, 0, fragment + 1,
						       fragment_len - 1);
			if (frag_offset < 0)
				return -FDT_ERR_BADOVERLAY;

			/* an __overlay__ subnode must exist */
			ret =
			    fdt_subnode_offset(fdto, frag_offset,
					       "__overlay__");
			if (ret < 0)
				return -FDT_ERR_BADOVERLAY;
		}
		rel_path = sep;
	} else {
		rel_path = fragment + path_len;
		fragment_len = path_len;
	}
	rel_path_len = snippet_len_o - (rel_path - snippet_o);

	if (rel_path_len <= 0 || fragment_len >= buflen)
		return -FDT_ERR_INTERNAL;

	ret =
	    lookup_target_path(fdt, fdto, fragment + 1, fragment_len - 1, buf,
			       buflen, NULL, &root_path);
	if (ret)
		return ret;

	/* Only single overlay should be present in path */
	if (find_node_str(buf, "__overlay__")
	    && find_node_str(rel_path, "__overlay__")) {
		if (sep)
			rel_path = sep + sizeof(olay) - 1;
	}

	rem = buflen - strlen(buf);
	if (rel_path_len >= rem)
		return -FDT_ERR_INTERNAL;

	sep = buf + strlen(buf);
	vvdprintf
	    ("%s: fdt-new_path(%s), sep(%s), rel_path(%s): hit-case %d, root_path %d\n",
	     __func__, buf, sep, rel_path, base_symbol_found, root_path);
	memcpy(sep, rel_path, rel_path_len);
	vdprintf
	    ("%s: fdt-final-new_path(%s), sep(%s), rel_path(%s): hit-case %d\n",
	     __func__, buf, sep, rel_path, base_symbol_found);

	return 0;
}

static const char *next_snippet(const char **prop,
				int *prop_len, int *snippet_len)
{
	const char *next = *prop;
	const char *tmp;
	int len;

	if (*prop_len <= 0)
		return NULL;

	tmp = memchr(next, '\0', *prop_len);
	if (!tmp)
		return NULL;

	tmp++;

	len = tmp - next;
	*snippet_len = len;
	*prop += len;
	*prop_len -= len;

	return next;
}

static int add_to_fixups(void *fdt, char *v, const char *label)
{
	const char *val;
	char *p;
	int vlen = strlen(v) + 1;	// 1 for NULL
	int len, ret;
	int root_fixup;

	root_fixup = fdt_subnode_offset(fdt, 0, "__fixups__");
	if (root_fixup == -FDT_ERR_NOTFOUND)
		root_fixup = fdt_add_subnode(fdt, 0, "__fixups__");

	if (root_fixup < 0)
		return root_fixup;

	val = fdt_getprop(fdt, root_fixup, label, &len);
	if (val)
		vlen += len;

	ret = fdt_setprop_placeholder(fdt, root_fixup, label,
				      vlen, (void **)&p);
	if (ret)
		return ret;

	if (val) {
		p += len;
		vlen -= len;
	}
	memcpy(p, v, vlen);

	return 0;
}

static int fdt_find_add_node(void *fdt, int parent_off, char *node)
{
	int offset;

	offset = fdt_subnode_offset(fdt, parent_off, node);
	if (offset < 0)
		offset = fdt_add_subnode(fdt, parent_off, node);

	return offset;
}

/* path => /abc/def/ghi */
static const char *next_node(const char **path, int *path_len, int *node_len)
{
	const char *sep = *path, *node;

	if (*sep != '/' || *path_len <= 0)
		return NULL;

	*path = *path + 1;
	node = *path;
	*path_len = *path_len - 1;

	sep = memchr(node, '/', *path_len);
	if (sep)
		*node_len = sep - node;
	else
		*node_len = *path_len;

	*path_len -= *node_len;
	*path += *node_len;

	return node;
}

static bool find_node_str(const char *path_str, const char *ip_str)
{
	const char *path, *node_str;
	int path_len = 0, node_len = 0, ip_str_len = 0;

	path = path_str;
	path_len = strlen(path);
	ip_str_len = strlen(ip_str);

	while ((node_str = next_node(&path, &path_len, &node_len))) {
		if (node_len >= ip_str_len
		    && !memcmp(node_str, ip_str, ip_str_len)) {
			vvdprintf("%s: Node(%s) present in path(%s)\n",
				  __func__, ip_str, path_str);
			return true;
		}
	}

	return false;
}

static int convert_to_u32(const char *p, uint32_t *val)
{
	char *endptr;
	unsigned long prop_val;

	prop_val = strtoul(p, &endptr, 10);
	if ((*endptr != '\0') || (endptr <= p))
		return -FDT_ERR_BADOVERLAY;

	*val = prop_val;	// size mis-match?

	return 0;
}

static int add_to_local_fixups(void *fdt, const char *snippet)
{
	const char *path, *prop_name, *prop_val, *node;
	int path_len, parent, ret, node_len, prop_len = 0;
	int snippet_len = strlen(snippet);
	uint32_t val = 0;
	char buf[MAX_BUF_SIZE];

	/* Validate format:
	 *      path_to_node : prop_name : prop_offset
	 *      OR
	 *      path_to_node
	 */
	path = snippet;
	prop_name = get_next_component(&snippet, &snippet_len, ':');

	if (prop_name) {
		prop_val = get_next_component(&snippet, &snippet_len, ':');
		if (!prop_val)
			return -FDT_ERR_BADOVERLAY;

		path_len = prop_name - path - 1;	// 1 for ':'
		prop_len = prop_val - prop_name - 1;	// 1 for ':'

		ret = convert_to_u32(prop_val, &val);
		if (ret)
			return ret;
	} else
		path_len = strlen(snippet);

	parent = fdt_find_add_node(fdt, 0, "__local_fixups__");
	if (parent < 0)
		return parent;

	vdprintf("%s: fdt-add snippet(%s)\n", __func__, snippet);
	while ((node = next_node(&path, &path_len, &node_len))) {
		int offset;

		offset =
		    fdt_subnode_offset_namelen(fdt, parent, node, node_len);
		if (offset < 0) {
			offset =
			    fdt_add_subnode_namelen(fdt, parent, node,
						    node_len);
			if (offset < 0)
				dprintf
				    ("%s: +fdt-add_subnode failed: node(%s), offset %d\n",
				     __func__, node, offset);
		}
		if (offset < 0)
			return offset;
		vdprintf
		    ("%s: ++fdt-subnode offset/add: node(%s), fdt-node(%s) offset %d\n",
		     __func__, node, fdt_get_name(fdt, offset, NULL), offset);
		parent = offset;
	}

	if (!prop_name)
		return parent;

	if (prop_len >= sizeof(buf))
		return -FDT_ERR_INTERNAL;
	memcpy(buf, prop_name, prop_len);
	buf[prop_len] = 0;
	if (ret >= prop_len)
		return -FDT_ERR_INTERNAL;

	vdprintf("%s: -fdt-appendprop: prop(%s), val (%d)\n", __func__, buf,
		 val);
	return fdt_appendprop_u32(fdt, parent, buf, val);
}

static int overlay_fixups_update(void *fdt, void *fdto,
				 uint32_t merge_olay_max_phdl)
{
	int ov_fixup, root_fixup, prop;

	ov_fixup = fdt_subnode_offset(fdto, 0, "__fixups__");
	if (ov_fixup < 0)
		return 0;

	root_fixup = find_add_subnode(fdt, 0, "__fixups__");
	if (root_fixup < 0)
		return root_fixup;

	fdt_for_each_property_offset(prop, fdto, ov_fixup) {
		int snippet_len, prop_len, base_symbol_found;
		const char *label, *snippet, *prop_val;

		prop_val = fdt_getprop_by_offset(fdto, prop, &label, &prop_len);
		if (prop_val == NULL)
			return -FDT_ERR_BADOVERLAY;

		base_symbol_found =
		    prop_exists_in_node(fdt, "/__symbols__", label);
		vdprintf
		    ("%s: Checking +fdto-prop-label(%s) val(%s), fdt-symbol-hit %d\n",
		     __func__, label, prop_val, base_symbol_found);

		while ((snippet =
			next_snippet(&prop_val, &prop_len, &snippet_len))) {
			char new_val[MAX_BUF_SIZE];
			int ignore = 0, ret;

			ret =
			    fixup_snippet_update(fdt, fdto, snippet,
						 snippet_len, new_val,
						 sizeof(new_val), &ignore,
						 base_symbol_found,
						 merge_olay_max_phdl);
			vdprintf
			    ("%s: +fixup_snippet(%s) label(%s) new_val(%s) "
			     "ret %d ignore %d\n", __func__, snippet, label,
			     ignore ? "NA" : new_val, ret, ignore);
			if (ret)
				return ret;

			if (ignore)
				continue;

			if (!base_symbol_found) {
				vdprintf
				    ("%s: non-hitcase: -fdt-add_to_fixups: "
				     "label(%s) new_val(%s) ret %d\n",
				     __func__, label, new_val, ret);
				ret = add_to_fixups(fdt, new_val, label);
			} else {
				vdprintf
				    ("%s: hitcase: -fdt-add_to_local_fixups: "
				     "new_val(%s) ret %d\n", __func__, new_val, ret);
				ret = add_to_local_fixups(fdt, new_val);
			}
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int overlay_local_fixups_update(void *fdt, void *fdto,
				       uint32_t merge_olay_max_phdl)
{
	int ov_lfixups, root_lfixups, node, ret;
	int root_path = 0;

	ov_lfixups = fdt_subnode_offset(fdto, 0, "__local_fixups__");
	if (ov_lfixups == -FDT_ERR_NOTFOUND)
		return 0;

	root_lfixups = fdt_subnode_offset(fdt, 0, "__local_fixups__");
	if (root_lfixups == -FDT_ERR_NOTFOUND)
		root_lfixups = fdt_add_subnode(fdt, 0, "__local_fixups__");

	if (root_lfixups < 0)
		return root_lfixups;

	fdt_for_each_subnode(node, fdto, ov_lfixups) {
		int len, child_node, parent_node;
		int skip_fdto_child = 0;
		const char *name = fdt_get_name(fdto, node, &len);
		char buf[MAX_BUF_SIZE];

		ret = lookup_target_path(fdt, fdto, name, strlen(name),
					 buf, sizeof(buf), NULL, &root_path);
		if (ret)
			return ret;

		/* We want to skip dtbo overlay child in two cases
		 * i.e When target path doesn't have fragment@ or __overlay__
		 * strings
		 */
		if (root_path || find_node_str(buf, "__overlay__"))
			skip_fdto_child = 1;

		parent_node = add_to_local_fixups(fdt, buf);
		if (parent_node < 0)
			return parent_node;

		child_node = fdt_subnode_offset(fdto, node, "__overlay__");
		if (child_node < 0)
			return -FDT_ERR_BADOVERLAY;

		vdprintf
		    ("%s: copy_node: In fdt-parent-path(%s) copy fdto-child-fragment(%s), "
		     "root_path %d\n", __func__, buf, name, root_path);
		vdprintf
		    ("%s: copy_node: In fdt-parent-node(%s) copy fdto-child-node(%s) "
		     "but skip-fdto-first-node=%d\n",
		     __func__, fdt_get_name(fdt, parent_node, NULL),
		     fdt_get_name(fdto, child_node, NULL), skip_fdto_child);
		ret = copy_node(fdt, fdto, parent_node, child_node,
				NULL, skip_fdto_child);
		if (ret)
			return ret;
	}

	return 0;
}

int fdt_overlay_merge(void *fdt, void *fdto, int *fdto_nospace)
{
	uint32_t delta = fdt_get_max_phandle(fdt);
	uint32_t delta0 = fdt_get_max_phandle(fdto);
	uint32_t max_phandle;
	int ret;

	fdt_check_header(fdt);
	fdt_check_header(fdto);

	*fdto_nospace = 0;

	if (UINT32_MAX - delta < delta0)
		return -FDT_ERR_BADOVERLAY;
	max_phandle = delta + delta0 + 1;
	dprintf("%s: *** delta %u & delta0 %u, max_phandle %u ***\n", __func__,
		delta, delta0, max_phandle);

	ret = overlay_adjust_local_phandles(fdto, delta);
	dprintf("%s: *** fdto-adjust_local_phandles, ret %d ***\n", __func__,
		ret);
	if (ret) {
		if (ret == -FDT_ERR_NOSPACE)
			*fdto_nospace = 1;
		goto err;
	}

	ret = overlay_update_local_references(fdto, delta);
	dprintf("%s: *** fdto-update_local_references, ret %d ***\n", __func__,
		ret);
	if (ret) {
		if (ret == -FDT_ERR_NOSPACE)
			*fdto_nospace = 1;
		goto err;
	}

	ret = overlay_fixup_phandles(fdt, fdto, max_phandle);
	dprintf("%s: *** fixup_phandles(1), ret %d ***\n", __func__, ret);
	if (ret && ret != -FDT_ERR_NOTFOUND)
		goto err;

	ret = overlay_merge(fdt, fdto, &max_phandle);
	dprintf("%s: *** merge(1), ret %d ***\n", __func__, ret);
	if (ret)
		goto err;

	/* local_fixups node is optional */
	max_phandle = delta + delta0 + 1;
	ret = overlay_symbol_update(fdt, fdto, max_phandle);
	dprintf("%s: *** symbol_update, ret %d ***\n", __func__, ret);
	if (ret)
		goto err;

	/* fixups node is optional */
	ret = overlay_fixups_update(fdt, fdto, max_phandle);
	dprintf("%s: *** fixups_update, ret %d ***\n", __func__, ret);
	if (ret < 0 && ret != -FDT_ERR_NOTFOUND)
		goto err;

	ret = overlay_local_fixups_update(fdt, fdto, max_phandle);
	dprintf("%s: *** local_fixups_update, ret %d ***\n", __func__, ret);
	if (ret < 0 && ret != -FDT_ERR_NOTFOUND)
		goto err;

	/*
	 * The overlay has been damaged, erase its magic.
	 */
	fdt_set_magic(fdto, ~0);

	dprintf("%s *** completed successfully! ***\n", __func__);
	return 0;

err:
	/*
	 * The overlay might have been damaged, erase its magic.
	 */
	fdt_set_magic(fdto, ~0);

	/*
	 * The base device tree might have been damaged, erase its
	 * magic.
	 */
	if (!*fdto_nospace)
		fdt_set_magic(fdt, ~0);
	dprintf("%s failed with ret %d ***\n", __func__, ret);

	return ret;
}
