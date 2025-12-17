#pragma once
#include <stdbool.h> 

#ifdef __cplusplus
extern "C"
{
#endif

    void bench_erase_run_100(bool confirm_whole_chip);
    void bench_erase_print_summary(void);
    bool bench_erase_has_data(void);

#ifdef __cplusplus
}
#endif
