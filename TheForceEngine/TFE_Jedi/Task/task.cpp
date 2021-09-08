#include "task.h"
#include <TFE_Memory/chunkedArray.h>
#include <TFE_DarkForces/time.h>
#include <TFE_System/system.h>
#include <stdarg.h>
#include <tuple>
#include <vector>

using namespace TFE_DarkForces;
using namespace TFE_Memory;

enum TaskConstants
{
	TASK_MAX_LEVELS = 16,	// Maximum number of recursion levels.
	TASK_INIT_LEVEL = -1,
};

struct TaskContext
{
	s32 ip[TASK_MAX_LEVELS];				// Current instruction pointer (IP) for each level of recursion.
	TaskFunc callstack[TASK_MAX_LEVELS];	// Funcion pointer for each level of recursion.

	u8* stackMem;		// starts out null, can point to the block if ever needed.
	u32 stackOffset;	// where in the stack we are.

	u8* stackPtr[TASK_MAX_LEVELS];
	u32 stackSize[TASK_MAX_LEVELS];
	u8  delayedCall[TASK_MAX_LEVELS];

	s32 level;
	s32 callLevel;
};

struct Task
{
#ifdef _DEBUG
	char name[32];
#endif

	Task* prevMain;
	Task* nextMain;
	Task* prevSec;
	Task* nextSec;
	Task* retTask;			// Task to return to once this one is completed or paused.
	void* userData;
	JBool framebreak;		// JTRUE if the task loop should end after this task.
	
	// Used in place of stack memory.
	TaskContext context;
	TaskFunc localRunFunc;

	// Timing.
	Tick nextTick;
	s32 activeIndex;
};

namespace TFE_Jedi
{
	enum
	{
		TASK_CHUNK_SIZE = 256,
		TASK_PREALLOCATED_CHUNKS = 1,

		TASK_STACK_SIZE = 64 * 1024,	// 64KB of stack memory.
		TASK_STACK_CHUNK_SIZE = 128,	// 8MB of memory for 128 tasks with stack memory.
	};

	ChunkedArray* s_tasks = nullptr;
	ChunkedArray* s_stackBlocks = nullptr;
	s32 s_taskCount = 0;

	Task  s_rootTask;
	Task* s_taskIter = nullptr;
	Task* s_curTask = nullptr;
	Task* s_resumeTask = nullptr;
	s32   s_currentId = -1;

	TaskContext* s_curContext = nullptr;

	void selectNextTask();

	void createRootTask()
	{
		s_tasks = createChunkedArray(sizeof(Task), TASK_CHUNK_SIZE, TASK_PREALLOCATED_CHUNKS);
		s_stackBlocks = createChunkedArray(TASK_STACK_SIZE, TASK_STACK_CHUNK_SIZE, TASK_PREALLOCATED_CHUNKS);

		s_rootTask = { 0 };
		s_rootTask.prevMain = &s_rootTask;
		s_rootTask.nextMain = &s_rootTask;
		s_rootTask.nextTick = TASK_SLEEP;

		s_taskIter = &s_rootTask;
		s_curTask = &s_rootTask;
		s_taskCount = 0;
	}

	Task* createTask(const char* name, TaskFunc func, TaskFunc localRunFunc)
	{
		if (!s_tasks)
		{
			createRootTask();
		}

		Task* newTask = (Task*)allocFromChunkedArray(s_tasks);
		assert(newTask);

		s_taskCount++;
#ifdef _DEBUG
		strcpy(newTask->name, name);
#endif
		newTask->nextMain = s_curTask->prevSec;
		newTask->prevMain = nullptr;
		newTask->nextSec = s_curTask;
		newTask->prevSec = nullptr;
		newTask->retTask = nullptr;
		newTask->userData = nullptr;
		newTask->framebreak = JFALSE;
		if (s_curTask->prevSec)
		{
			s_curTask->prevSec->prevMain = newTask;
		}
		s_curTask->prevSec = newTask;

		newTask->nextTick = 0;

		newTask->context = { 0 };
		newTask->context.callstack[0] = func;
		newTask->localRunFunc = localRunFunc;
		newTask->context.level = TASK_INIT_LEVEL;
		return newTask;
	}

	Task* pushTask(const char* name, TaskFunc func, JBool framebreak, TaskFunc localRunFunc)
	{
		if (!s_tasks)
		{
			createRootTask();
		}

		Task* newTask = (Task*)allocFromChunkedArray(s_tasks);
		assert(newTask);

		s_taskCount++;
		// Insert the task after 's_taskIter'
#ifdef _DEBUG
		strcpy(newTask->name, name);
#endif
		newTask->nextMain = s_taskIter->nextMain;
		newTask->prevMain = s_taskIter;
		s_taskIter->nextMain = newTask;

		newTask->prevSec = nullptr;
		newTask->nextSec = nullptr;
		newTask->retTask = nullptr;
		newTask->userData = nullptr;
		newTask->framebreak = framebreak;

		newTask->context = { 0 };
		newTask->context.callstack[0] = func;
		newTask->localRunFunc = localRunFunc;
		newTask->context.level = TASK_INIT_LEVEL;
		newTask->nextTick = s_curTick;

		return newTask;
	}

	Task* task_getCurrent()
	{
		return s_curTask;
	}

	void task_free(Task* task)
	{
		s_taskCount--;
		for (s32 i = 0; i < s_taskCount; i++)
		{
			Task* itask = (Task*)chunkedArrayGet(s_tasks, i);
			if (itask != task)
			{
				if (itask->prevMain == task)
				{
					itask->prevMain = task->prevMain;
				}
				if (itask->nextMain == task)
				{
					itask->nextMain = task->nextMain;
				}
				if (itask->prevSec == task)
				{
					itask->prevSec = task->prevSec;
				}
				if (itask->nextSec == task)
				{
					itask->nextSec = task->nextSec;
				}
			}
		}

		if (task == s_curTask)
		{
			selectNextTask();
		}

		// Free any memory allocated for the local context.
		freeToChunkedArray(s_stackBlocks, task->context.stackMem);
		// Finally free the task itself from the chunked array.
		freeToChunkedArray(s_tasks, task);
	}

	void task_freeAll()
	{
		chunkedArrayClear(s_tasks);
		chunkedArrayClear(s_stackBlocks);

		s_curTask    = nullptr;
		s_curContext = nullptr;
		s_taskCount  = 0;
	}

	void task_shutdown()
	{
		freeChunkedArray(s_tasks);
		freeChunkedArray(s_stackBlocks);

		s_curTask     = nullptr;
		s_tasks       = nullptr;
		s_stackBlocks = nullptr;
		s_curContext  = nullptr;
		s_taskCount   = 0;
	}

	void task_makeActive(Task* task)
	{
		task->nextTick = 0;
	}

	void task_setNextTick(Task* task, Tick tick)
	{
		task->nextTick = tick;
	}

	void task_setUserData(Task* task, void* data)
	{
		task->userData = data;
	}

	void task_runLocal(Task* task, s32 id)
	{
		if (task->localRunFunc)
		{
			task->localRunFunc(id);
		}
	}

	void ctxReturn()
	{
		s32 level = 0;
		if (s_curContext)
		{
			level = s_curContext->level;
			assert(s_curContext->level >= 0 && s_curContext->level < TASK_MAX_LEVELS);
			if (level <= 0 || !s_curContext->delayedCall[level-1])
			{
				s_curContext->level--;
			}
			else
			{
				// We need to reduce by 2 levels since a delayed call does not immediately return to the calling function.
				s_curContext->level -= 2;
			}
			assert(s_curContext->level >= -1 && s_curContext->level < TASK_MAX_LEVELS);

			if (s_curContext->callLevel > 0) { s_curContext->callLevel--; }
		}

		if (level == 0 && s_curTask)
		{
			task_free(s_curTask);
			s_curTask = nullptr;
		}
		else if (s_curContext && s_curContext->stackPtr[level])
		{
			// Return the stack memory allocated for this level.
			s_curContext->stackOffset -= s_curContext->stackSize[level];
			assert(s_curContext->stackOffset >= 0 && s_curContext->stackOffset < TASK_STACK_SIZE);
			s_curContext->stackPtr[level] = nullptr;
			s_curContext->stackSize[level] = 0;
		}
	}

	void selectNextTask()
	{
		// Find the next task to run.
		Task* task = s_curTask;
		while (1)
		{
			if (task->nextMain)
			{
				task = task->nextMain;
				while (task->prevSec)
				{
					task = task->prevSec;
				}
				if (task->nextTick <= s_curTick || task->framebreak)
				{
					s_currentId = 0;
					s_curTask = task;
					return;
				}
			}
			else if (task->nextSec)
			{
				task = task->nextSec;
				if (task->nextTick <= s_curTick || task->framebreak)
				{
					s_currentId = 0;
					s_curTask = task;
					return;
				}
			}
			else
			{
				break;
			}
		}
	}

	void itask_run(Task* task, s32 id)
	{
		task->retTask = s_curTask;
		s_currentId = id;
		s_curTask = task;

		s_curContext = &s_curTask->context;
		// Save the current recursion level.
		s32 prevLevel = s_curContext->level;

		// When a task is run directly, it is called in-place since control needs to be handed over immediately.
		// When yield is called, control will pass back to the calling task.
		// Call from the base level, we are starting again.
		s_curContext->level = -1;
		s_curContext->ip[0] = 0;
		s32 level = max(0, s_curContext->level);
		TaskFunc runFunc = s_curContext->callstack[level];
		assert(runFunc);
		if (runFunc)
		{
			runFunc(s_currentId);
		}

		// Restore the previous level.
		if (prevLevel != task->context.level)
		{
			task->context.level = prevLevel;
		}
	}

	void itask_yield(Tick delay, s32 ip)
	{
		// Copy the ip so we know where to return.
		assert(s_curContext->level >= 0 && s_curContext->level < TASK_MAX_LEVELS);
		s_curContext->ip[s_curContext->level] = ip;
		s_curContext->level--;

		// If there is a return task, then take it next.
		if (s_curTask->retTask)
		{
			// Clear out the return task once it is executed.
			Task* retTask = s_curTask->retTask;
			s_curTask->retTask = nullptr;

			// Set the next task.
			s_currentId = 0;
			s_curTask = retTask;
			s_curContext = &s_curTask->context;
			return;
		}

		// Update the current tick based on the delay.
		s_curTask->nextTick = (delay < TASK_SLEEP) ? s_curTick + delay : delay;
		
		// Find the next task to run.
		selectNextTask();
		assert(s_curTask);
	}
		
	// Called once per frame to run all of the tasks.
	void task_run()
	{
		if (!s_taskCount)
		{
			return;
		}

		// Find the next task to run.
		Task* task = s_resumeTask ? s_resumeTask : s_curTask;
		do
		{
			if (task->nextMain)
			{
				task = task->nextMain;
			}
			else if (task->nextSec)
			{
				task = task->nextSec;
			}
		} while (!task || !task->context.callstack[0]);	// loop as long as task is null or the task has no valid callstack (i.e. the root).
		s_curTask = task;
		s_currentId = 0;

		// Keep processing tasks until the "framebreak" task is hit.
		// Once the framebreak task completes (if it is not sleeping), then break out of the loop - processing will resume
		// on the next task on the next frame.
		// Note: the original code just loop here forever, but we break it up between frames to play nice with modern operating systems.
		while (s_curTask)
		{
			JBool framebreak = s_curTask->framebreak;
			if (framebreak)
			{
				s_resumeTask = s_curTask;
			}

			// This should only be false when hitting the "framebreak" task which is sleeping.
			if (s_curTask->nextTick <= s_curTick)
			{
				s_curContext = &s_curTask->context;
				s32 level = max(0, s_curContext->level + 1);
				TaskFunc runFunc = s_curContext->callstack[level];
				assert(runFunc);

				if (runFunc)
				{
					runFunc(s_currentId);
				}
			}
			else if (!framebreak)
			{
				selectNextTask();
			}

			if (framebreak)
			{
				break;
			}
		}
	}

	void task_setDefaults()
	{
		s_curTask  = &s_rootTask;
		s_taskIter = &s_rootTask;
	}

	s32 task_getCount()
	{
		return s_taskCount;
	}

	s32 ctxGetIP()
	{
		assert(s_curContext->level >= 0 && s_curContext->level < TASK_MAX_LEVELS);
		return s_curContext->ip[s_curContext->level];
	}

	void ctxAllocate(u32 size)
	{
		if (!size) { return; }
		if (!s_curContext->stackMem)
		{
			s_curContext->stackMem = (u8*)allocFromChunkedArray(s_stackBlocks);
			memset(s_curContext->stackSize, 0, sizeof(u32) * TASK_MAX_LEVELS);
			s_curContext->stackOffset = 0;
		}

		s32 level = s_curContext->level;
		if (!s_curContext->stackPtr[level])
		{
			s_curContext->stackPtr[level] = s_curContext->stackMem + s_curContext->stackOffset;
			s_curContext->stackSize[level] = size;
			s_curContext->stackOffset += size;
			assert(s_curContext->stackOffset >= 0 && s_curContext->stackOffset < TASK_STACK_SIZE);

			// Clear out the memory.
			memset(s_curContext->stackPtr[level], 0, size);
		}
	}

	void* ctxGet()
	{
		return s_curContext->stackPtr[s_curContext->level];
	}

	void ctxBegin()
	{
		s_curContext->level++;
	}

	// Direct calls from a task are a bit complicated, especially when those calls can yield.
	// This task needs to track the current IP at the calling level so it can be resumed when the new function returns.
	// In addition, we must detect when the return is delayed - due to a yield in the called function - 
	// so that the recursion level is properly handled on delayed return.
	bool ctxCall(TaskFunc func, s32 id, s32 ip)
	{
		assert(s_curContext->level >= 0 && s_curContext->level + 1 < TASK_MAX_LEVELS);
		TaskContext* startContext = s_curContext;
		if (s_curContext->level == 0)
		{
			s_curContext->callLevel = 0;
		}
		s32 startLevel = s_curContext->level;
		s32 startCallLevel = s_curContext->callLevel;
		startContext->delayedCall[startLevel] = 0;
		s_curContext->callLevel++;

		s_curContext->ip[s_curContext->level] = ip;
		s_curContext->callstack[s_curContext->level + 1] = func;
		s_curContext->ip[s_curContext->level + 1] = 0;
		func(id);

		if (startCallLevel != startContext->callLevel)
		{
			startContext->delayedCall[startLevel] = 1;
		}
		return startCallLevel != startContext->callLevel;
	}
}