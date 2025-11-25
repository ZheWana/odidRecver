#include <fcntl.h> // O_CREAT, O_WRONLY
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
  const char *queue_name = "/myqueue"; // POSIX 消息队列名字必须以 / 开头
  mqd_t mq;
  struct mq_attr attr;
  struct student stu;
  int ret;

  // 设置队列属性
  memset (&attr, 0, sizeof (attr));
  attr.mq_flags = 0;                         // 0 表示阻塞模式
  attr.mq_maxmsg = 10;                       // 队列中最多 10 条消息
  attr.mq_msgsize = sizeof (struct student); // 单条消息大小
  attr.mq_curmsgs = 0; // 当前消息条数（只读字段，mq_open 时忽略）

  // 打开（或创建）消息队列
  mq = mq_open (queue_name, O_CREAT | O_WRONLY, 0666, &attr);
  if (mq == (mqd_t)-1)
    {
      perror ("mq_open");
      exit (EXIT_FAILURE);
    }

  // 准备要发送的结构体
  stu.id = 1001;
  strncpy (stu.name, "ZhangSan", sizeof (stu.name) - 1);
  stu.name[sizeof (stu.name) - 1] = '\0';
  stu.score = 95.5f;

  // 发送消息
  // 第 4 个参数是优先级 priority（0~MQ_PRIO_MAX-1），这里用 0 即可
  ret = mq_send (mq, (const char *)&stu, sizeof (stu), 0);
  if (ret == -1)
    {
      perror ("mq_send");
      mq_close (mq);
      exit (EXIT_FAILURE);
    }

  printf ("发送成功: id=%d, name=%s, score=%.2f\n", stu.id, stu.name,
          stu.score);

  // 关闭消息队列
  mq_close (mq);

  // 注意：不在发送端删除队列（mq_unlink），否则接收端可能找不到。
  // 队列的删除可以在接收端做，或者单独弄个清理程序。

  return 0;
}