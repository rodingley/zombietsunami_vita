#ifndef JNI_FAKE_H
#define JNI_FAKE_H

typedef struct jobj jobj;

void jni_init(void);
void jni_dump_stats(void);
void *jni_vm(void);  /* JavaVM*  */
void *jni_env(void); /* JNIEnv*  */
jobj *jni_find_class(const char *name);
jobj *jni_new_object(const char *class_name);
jobj *jni_new_string(const char *utf8);
const char *jni_string_utf(void *s); /* contents of a string object, or NULL */
/* a real-money purchase the game asked for, to be answered "failed" (1 = one is waiting) */
int jni_take_failed_purchase(char *sku, unsigned n);

#endif
