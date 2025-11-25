#include <iostream>

#define ENABLE_INFO_LOG 1
#define ENABLE_ERROR_LOG 1

#define log_info(msg)                                                         \
  if (ENABLE_INFO_LOG)                                                        \
  std::cout << "[INFO] " << msg << std::endl
#define log_err(msg)                                                          \
  if (ENABLE_ERROR_LOG)                                                       \
  std::cout << "[ERROR] " << __FILE__ << ":" << __LINE__ << msg << std::endl
