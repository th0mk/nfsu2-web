/* Switch: no QoS classes */
#pragma once
#include <pthread.h>
typedef int qos_class_t;
#define QOS_CLASS_USER_INTERACTIVE 0x21
#define QOS_CLASS_USER_INITIATED   0x19
#define QOS_CLASS_DEFAULT          0x15
#define QOS_CLASS_UTILITY          0x11
#define QOS_CLASS_BACKGROUND       0x09
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int p){ (void)a; (void)q; (void)p; return 0; }
static inline int pthread_set_qos_class_self_np(qos_class_t q, int p){ (void)q; (void)p; return 0; }
