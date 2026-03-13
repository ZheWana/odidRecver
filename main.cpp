#include "Recver.hpp"
#include <iostream>

int
main (int argc, char *argv[])
{
  Recver recver (0,     // id
                 100,   // expectedDwellTime_
                 100,    // scheduleTimeoutTime_
                 1000); // minScheduleInterval_
  recver.channelManagementLoop ();
  return 0;
}