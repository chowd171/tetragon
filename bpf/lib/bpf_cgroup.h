// SPDX-License-Identifier: GPL-2.0
/* Copyright Authors of Tetragon */

#ifndef __BPF_CGROUP_
#define __BPF_CGROUP_

#include "hubble_msg.h"
#include "bpf_helpers.h"
#include "environ_conf.h"

#define NULL ((void *)0)

/* Our kernfs node name length, can be made 256? */
#define KN_NAME_LENGTH 128

/* Represent old kernfs node with the kernfs_node_id
 * union to read the id in 5.4 kernels and older
 */
struct kernfs_node___old {
	union kernfs_node_id id;
};

/**
 * get_cgroup_kn_name() Returns a pointer to the kernfs node name
 * @cgrp: target kernfs node
 *
 * Returns a pointer to the kernfs node name on success, NULL on failures.
 */
static inline __attribute__((always_inline)) const char *
__get_cgroup_kn_name(const struct kernfs_node *kn)
{
	const char *name = NULL;

	if (kn)
		probe_read(&name, sizeof(name), _(&kn->name));

	return name;
}

/**
 * get_cgroup_kn_id() Returns the kernfs node id
 * @cgrp: target kernfs node
 *
 * Returns the kernfs node id on success, zero on failures.
 */
static inline __attribute__((always_inline)) __u64
__get_cgroup_kn_id(const struct kernfs_node *kn)
{
	__u64 id = 0;
	struct kernfs_node___old *old_kn;

	if (!kn)
		return id;

	/* Kernels prior to 5.5 have the kernfs_node_id */
	if (!bpf_core_type_exists(union kernfs_node_id)) {
		probe_read(&id, sizeof(id), _(&kn->id));
	} else {
		old_kn = (void *)kn;
		if (BPF_CORE_READ_INTO(&id, old_kn, id.id) != 0)
			return 0;
	}

	return id;
}

/**
 * __get_cgroup_kn() Returns the kernfs_node of the cgroup
 * @cgrp: target cgroup
 *
 * Returns the kernfs_node of the cgroup on success, NULL on failures.
 */
static inline __attribute__((always_inline)) struct kernfs_node *
__get_cgroup_kn(const struct cgroup *cgrp)
{
	struct kernfs_node *kn = NULL;

	if (cgrp)
		probe_read(&kn, sizeof(cgrp->kn), _(&cgrp->kn));

	return kn;
}

/**
 * get_cgroup_hierarchy_id() Returns the cgroup hierarchy id
 * @cgrp: target cgroup
 *
 * Returns the cgroup hierarchy id. Make sure you pass a valid
 * cgroup, this can not fail.
 *
 * Returning zero means the cgroup is running on the default
 * hierarchy.
 */
static inline __attribute__((always_inline)) __u32
get_cgroup_hierarchy_id(const struct cgroup *cgrp)
{
	__u32 id;

	BPF_CORE_READ_INTO(&id, cgrp, root, hierarchy_id);

	return id;
}

/**
 * get_cgroup_name() Returns a pointer to the cgroup name
 * @cgrp: target cgroup
 *
 * Returns a pointer to the cgroup node name on success that can
 * be read with probe_read(). NULL on failures.
 */
static inline __attribute__((always_inline)) const char *
get_cgroup_name(const struct cgroup *cgrp)
{
	const char *name;

	if (unlikely(!cgrp))
		return NULL;

	if (BPF_CORE_READ_INTO(&name, cgrp, kn, name) != 0)
		return NULL;

	return name;
}

/**
 * get_cgroup_id() Returns cgroup id
 * @cgrp: target cgroup
 *
 * Returns the cgroup id of the target cgroup on success, zero on failures.
 */
static inline __attribute__((always_inline)) __u64
get_cgroup_id(const struct cgroup *cgrp)
{
	struct kernfs_node *kn;

	kn = __get_cgroup_kn(cgrp);
	return __get_cgroup_kn_id(kn);
}

/**
 * get_task_cgroup() Returns the accurate or desired cgroup of the css of
 *    current task that we want to operate on.
 * @task: must be current task.
 * @subsys_idx: index of the desired cgroup_subsys_state part of css_set.
 *    Passing a zero as a subsys_idx is fine assuming you want that.
 *
 * Returns the cgroup of the css part of css_set of current task and is
 * indexed at subsys_idx on success, NULL on failures.
 *
 * To get cgroup and kernfs node information we want to operate on the right
 * cgroup hierarchy which is setup by user space. However due to the
 * incompatiblity between cgroup v1 and v2; how user space initialize and
 * install cgroup controllers, etc, it can be difficult.
 *
 * Use this helper and pass the css index that you consider accurate and
 * which can be discovered at runtime in user space.
 * Usually it is the 'memory' or 'pids' indexes by reading /proc/cgroups
 * file where each line number is the index starting from zero without
 * counting first comment line.
 */
static inline __attribute__((always_inline)) struct cgroup *
get_task_cgroup(struct task_struct *task, __u32 subsys_idx)
{
	struct cgroup_subsys_state *subsys;
	struct css_set *cgroups;
	struct cgroup *cgrp = NULL;

	probe_read(&cgroups, sizeof(cgroups), _(&task->cgroups));
	if (unlikely(!cgroups))
		return cgrp;

	/* We are interested only in the cpuset, memory or pids controllers
	 * which are indexed at 0, 4 and 11 respectively assuming all controllers
	 * are compiled in.
	 * When we use the controllers indexes we will first discover these indexes
	 * dynamically in user space which will work on all setups from reading
	 * file: /proc/cgroups. If we fail to discover the indexes then passing
	 * a default index zero should be fine assuming we also want that.
	 *
	 * Reference: https://elixir.bootlin.com/linux/v5.19/source/include/linux/cgroup_subsys.h
	 *
	 * Notes:
	 * Newer controllers should be appended at the end. controllers
	 * that are not upstreamed may mess the calculation here
	 * especially if they happen to be before the desired subsys_idx,
	 * we fail.
	 */
	if (unlikely(subsys_idx > pids_cgrp_id))
		return cgrp;

	/* Read css from the passed subsys index to ensure that we operate
	 * on the desired controller. This allows user space to be flexible
	 * and chose the right per cgroup subsystem to use in order to
	 * support as much as workload as possible. It also reduces errors
	 * in a significant way.
	 */
	probe_read(&subsys, sizeof(subsys), _(&cgroups->subsys[subsys_idx]));
	if (unlikely(!subsys))
		return cgrp;

	probe_read(&cgrp, sizeof(cgrp), _(&subsys->cgroup));
	return cgrp;
}

#endif // __BPF_CGROUP_
