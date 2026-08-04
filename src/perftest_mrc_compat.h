#ifndef PERFTEST_MRC_COMPAT_H
#define PERFTEST_MRC_COMPAT_H

#ifdef HAVE_MRC_API_V1_0_1
#ifndef MRC_API_VER_USED
#define MRC_API_VER_USED MRC_API_VER(1, 0, 1)
#endif
#endif

#include MRC_PATH

#ifdef HAVE_MRC_DEVICE_ATTR
#define PERFTEST_MRC_DEVICE_ATTR struct mrc_device_attr
#else
#define PERFTEST_MRC_DEVICE_ATTR struct mrc_attr
#endif

#endif /* PERFTEST_MRC_COMPAT_H */
