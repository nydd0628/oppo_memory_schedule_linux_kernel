/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_ANON_PAGE_ADMISSION_H
#define _LINUX_ANON_PAGE_ADMISSION_H

#include <linux/mm_types.h>

enum anon_admission_action {
	ANON_ADMISSION_PASS,
	ANON_ADMISSION_RETRY,
	ANON_ADMISSION_ALLOCATE,
	ANON_ADMISSION_MEASURE_TOP,
	ANON_ADMISSION_MEASURE_FGS,
};

#ifdef CONFIG_ANON_PAGE_ADMISSION
#include <linux/jump_label.h>

DECLARE_STATIC_KEY_FALSE(anon_admission_key);
extern bool anon_admission_pressure_active;

static inline bool anon_admission_active(void)
{
	return static_branch_unlikely(&anon_admission_key) &&
		READ_ONCE(anon_admission_pressure_active);
}

enum anon_admission_action anon_admission_prepare(struct vm_fault *vmf);
bool anon_admission_wait_fault(void);
void anon_admission_finish(enum anon_admission_action action, bool success);
void anon_admission_cancel_fault(void);
void anon_admission_end_fault(void);
bool anon_admission_domain_matches(vm_flags_t vm_flags);
bool anon_admission_zone_margin(long *low, long *high);
#else
static inline bool anon_admission_active(void) { return false; }
static inline enum anon_admission_action
anon_admission_prepare(struct vm_fault *vmf) { return ANON_ADMISSION_PASS; }
static inline bool anon_admission_wait_fault(void) { return false; }
static inline void anon_admission_finish(enum anon_admission_action action,
					 bool success) {}
static inline void anon_admission_cancel_fault(void) {}
static inline void anon_admission_end_fault(void) {}
#endif

static inline void anon_admission_cancel(unsigned int flags)
{
	if (flags & FAULT_FLAG_ANON_ADMISSION)
		anon_admission_cancel_fault();
}

#endif /* _LINUX_ANON_PAGE_ADMISSION_H */
