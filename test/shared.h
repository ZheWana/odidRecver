#ifndef SHARED_H
#define SHARED_H
#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct
  {
    char name[10];
    int id;
    float score;
  } shared_data_t;

#define SHARED_MEMORY_NAME "/my_shared_memory"
#define SHARED_MEMORY_SIZE sizeof (shared_data_t)

#ifdef __cplusplus
}
#endif
#endif // SHARED_H