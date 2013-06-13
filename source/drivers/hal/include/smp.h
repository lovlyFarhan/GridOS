/**
*   See the readme.txt at the root directory of this project for the idea and originality of this operating system.
*   See the license.txt at the root directory of this project for the copyright information about this file and project.
*
* 
*   HAL SMP
*/

#ifndef HAL_SMP_H
#define HAL_SMP_H

static inline int raw_smp_processor_id()
{
	//TODO to support smp
	return 0;
}
#define smp_processor_id() raw_smp_processor_id()

#endif
