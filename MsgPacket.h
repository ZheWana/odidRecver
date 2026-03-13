#ifndef __INFO_MSG_H__
#define __INFO_MSG_H__

#define PACKET_NAME_MAX_SIZE 25

#ifdef __cplusplus
extern "C"
{
#endif
#include "stdint.h"
  typedef struct MsgPacket
  {
    char ID[PACKET_NAME_MAX_SIZE];
    uint16_t channel_freq;
    int timestep;
  } msgPacket_t;

#ifdef __cplusplus
}
#endif

#define MSG_QUEUE_NAME "/odid_msg_queue"
#define MSG_QUEUE_MAX_MSG 100
#define MSG_QUEUE_MSG_SIZE sizeof (msgPacket_t)

#endif // __INFO_MSG_H__
