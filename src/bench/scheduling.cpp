#include "lob/bench/scheduling.hpp"

#include <pthread.h>
#include <pthread/qos.h>

namespace lob::bench {

bool SetInteractiveQos() {
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
}

std::size_t CurrentCpuNumber() {
    std::size_t cpu = 0;
    pthread_cpu_number_np(&cpu);
    return cpu;
}

}  // namespace lob::bench
