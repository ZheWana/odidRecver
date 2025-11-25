#include <fcntl.h> // O_RDONLY
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> // mode_t

struct student
{
  int id;
  char name[32];
  float score;
};

int
main ()
{
  const char *queue_name = "/myqueue";
  mqd_t mq;
  struct mq_attr attr;
  struct student stu;
  ssize_t bytes_read;
  int ret;

  // 打开已经存在的消息队列（只读）
  mq = mq_open (queue_name, O_RDONLY);
  if (mq == (mqd_t)-1)
    {
      perror ("mq_open");
      exit (EXIT_FAILURE);
    }

  // 可选：获取队列属性，查看消息大小等
  ret = mq_getattr (mq, &attr);
  if (ret == -1)
    {
      perror ("mq_getattr");
      mq_close (mq);
      exit (EXIT_FAILURE);
    }

  if (attr.mq_msgsize < sizeof (struct student))
    {
      fprintf (stderr, "队列的单条消息大小太小: mq_msgsize=%ld, 需要=%zu\n",
               attr.mq_msgsize, sizeof (struct student));
      mq_close (mq);
      exit (EXIT_FAILURE);
    }

  // 接收消息（这里假设肯定会收到一个完整的 student）
  bytes_read = mq_receive (mq, (char *)&stu, sizeof (stu), NULL);
  if (bytes_read == -1)
    {
      perror ("mq_receive");
      mq_close (mq);
      exit (EXIT_FAILURE);
    }

  printf ("接收成功: id=%d, name=%s, score=%.2f (接收到字节数=%zd)\n", stu.id,
          stu.name, stu.score, bytes_read);

  // 关闭队列
  mq_close (mq);

  // 删除消息队列（类似 unlink 文件）
  // 一般由“最后使用完”的一方负责删除
  if (mq_unlink (queue_name) == -1)
    {
      perror ("mq_unlink");
      // 即使删除失败，程序也可以正常结束
    }

  return 0;
}