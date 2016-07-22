#include "VirtualMachine.h"
#include "Machine.h"
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>
#include <queue>

using namespace std;

typedef struct // Thread Control Block
{
  SMachineContext machineContext; // The context of the machine, basically thread state
  TVMThreadEntry entry; // This is a function apparently
  void *param; // Holds any parameters that might be sent to the entry function
  void *stackaddr; // Pointer to the stack
  TVMMemorySize stacksize; // Size of said stack
  TVMThreadPriority priority; // The priority of the thread to be executed
  TVMThreadState state; // The state of the thread
  volatile int sleepCount; // The sleep timer, for every individual thread
} TCB; 

typedef struct 
{
  bool lock; // If a mutex is locked by a thread
  TVMThreadID owner; // the ID of the thread that owns this mutex
  queue<TVMThreadID> waitingHigh; // High priority threads waiting
  queue<TVMThreadID> waitingNormal; // Medium priority threads waiting
  queue<TVMThreadID> waitingLow; // Low priority threads waiting
} Mutex;

volatile TVMTick ticks;
volatile TVMTick totalticks;
map<TVMThreadID, TCB*> threadMap;
map<TVMMutexID, Mutex*> mutexMap;
queue<TVMThreadID> readyHigh;
queue<TVMThreadID> readyNormal;
queue<TVMThreadID> readyLow;

extern "C" 
{

bool anythingReady()
{
  int size = threadMap.size();

  for (int i = 0; i < size; i++)
    if (threadMap[i] -> state == VM_THREAD_STATE_READY)
      return true;

  return false;
}

void printStates()
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  int size = threadMap.size();

  cerr << "States" << endl << "-------------------" << endl;
  for (int i = 0; i < size; i++)
  {
    cerr << i << " : " ;
    if (threadMap[i] -> state == 0)
      cerr << "Dead" << endl;

    if (threadMap[i] -> state == 1)
      cerr << "Running" << endl;

    if (threadMap[i] -> state == 2)
      cerr << "Ready" << endl;

    if (threadMap[i] -> state == 3)
      cerr << "Waiting" << endl;
  }
  cerr << "-------------------" << endl;
  MachineResumeSignals(&signals);
}

void sort(TVMThreadID thread)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  if (threadMap[thread] -> priority == VM_THREAD_PRIORITY_LOW)
    readyLow.push(thread);

  else if (threadMap[thread] -> priority == VM_THREAD_PRIORITY_NORMAL)
    readyNormal.push(thread);

  else if (threadMap[thread] -> priority == VM_THREAD_PRIORITY_HIGH)
    readyHigh.push(thread);

  else 
  {
    MachineResumeSignals(&signals);
    return;
  }

  MachineResumeSignals(&signals);
  return;
}

void mutexSort(TVMThreadID thread, TVMMutexID mutex)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  // cerr << "Inserting thread: " << thread << endl;
  if (threadMap[thread] -> priority == VM_THREAD_PRIORITY_LOW)
    (mutexMap[mutex] -> waitingLow).push(thread);

  else if (threadMap[thread] -> priority == VM_THREAD_PRIORITY_NORMAL)
    (mutexMap[mutex] -> waitingNormal).push(thread);

  else if (threadMap[thread] -> priority == VM_THREAD_PRIORITY_HIGH)
    (mutexMap[mutex] -> waitingHigh).push(thread);

  else 
  {
    MachineResumeSignals(&signals);
    return;
  }

  MachineResumeSignals(&signals);
  return;
}

TVMThreadID findNextThread()
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  TVMThreadID nextThread;

  if (readyHigh.size() > 0)
  {
    nextThread = readyHigh.front();
    readyHigh.pop();
    MachineResumeSignals(&signals);
    return nextThread;
  }

  else if (readyNormal.size() > 0)
  {
    nextThread = readyNormal.front();
    readyNormal.pop();
    MachineResumeSignals(&signals);
    return nextThread;
  }

  else if (readyLow.size() > 0)
  {
    nextThread = readyLow.front();
    readyLow.pop();
    MachineResumeSignals(&signals);
    return nextThread;
  }

  MachineResumeSignals(&signals);
  return 1;
}

TVMThreadID findNextThreadMutex(TVMMutexID mutex)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  TVMThreadID nextThread;

  if ((mutexMap[mutex] -> waitingHigh).size() > 0)
  {
    nextThread = (mutexMap[mutex] -> waitingHigh).front();
    (mutexMap[mutex] -> waitingHigh).pop();
    MachineResumeSignals(&signals);
    return nextThread;
  }

  else if ((mutexMap[mutex] -> waitingNormal).size() > 0)
  {
    nextThread = (mutexMap[mutex] -> waitingNormal).front();
    (mutexMap[mutex] -> waitingNormal).pop();
    MachineResumeSignals(&signals);
    return nextThread;
  }

  else if ((mutexMap[mutex] -> waitingLow).size() > 0)
  {
    nextThread = (mutexMap[mutex] -> waitingLow).front();
    (mutexMap[mutex] -> waitingLow).pop();
    MachineResumeSignals(&signals);
    return nextThread;
  }

  MachineResumeSignals(&signals);
  return 1;
}


void VMThreadScheduler(TVMThreadState targetState)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  TVMThreadID currentID;
  VMThreadID(&currentID);
  // cerr << "Running thread is: " << currentID << endl;
  TVMThreadID nextThread;
  nextThread = findNextThread();
  // printStates();
  
  if (currentID == nextThread)
  {
    MachineResumeSignals(&signals);
    return;
  }

  if ((targetState == VM_THREAD_STATE_READY) && (threadMap[currentID] -> priority == threadMap[nextThread] -> priority))
  {
    sort(nextThread);
    MachineResumeSignals(&signals);
    return;
  }

  threadMap[currentID] -> state = targetState;

  if (targetState == VM_THREAD_STATE_READY)
    sort(currentID);

  MachineContextSave(&(threadMap[currentID] -> machineContext));

  // cerr << "Switching from " << currentID << " to " << nextThread << endl;
  // cerr << "Target thread priority: " << threadMap[nextThread] -> priority << endl;

  threadMap[nextThread] -> state = VM_THREAD_STATE_RUNNING;
  // printStates();

  MachineContextSwitch(&(threadMap[currentID] -> machineContext), &(threadMap[nextThread] -> machineContext));
  // cerr << "Switched successfully." << endl;
  // MachineContextRestore(threadMap[nextThread] -> machineContext);
  MachineResumeSignals(&signals);
  return;
}

void VMMutexScheduler(TVMMutexID mutex)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  TVMThreadID nextThread = findNextThreadMutex(mutex);

  if (nextThread == 1)
  {
    MachineResumeSignals(&signals);
    return;
  }
  else
  {
    mutexMap[mutex] -> owner = nextThread;
    mutexMap[mutex] -> lock = true;
    threadMap[nextThread] -> state = VM_THREAD_STATE_READY;
    // cerr << "nextThread in VMMutexScheduler: " << nextThread << endl;
    sort(nextThread);
    VMThreadScheduler(VM_THREAD_STATE_READY);
    MachineResumeSignals(&signals);
    return;
  }

  MachineResumeSignals(&signals);
  return;
}

void openCallback(void *calldata, int result)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  callreturn = result;
  callstatus = 1;
  MachineResumeSignals(&signals);
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
{
  callstatus = 0;
  
  MachineFileOpen (filename, flags, mode, openCallback, NULL);

  while(callstatus == 0){ /* Waiting */ }
  
  *filedescriptor= callreturn;
  
  return VM_STATUS_SUCCESS;
  // wait till a flag is set in the callback
}

void closeCallback(void *calldata, int result)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  callstatus = 1;
  MachineResumeSignals(&signals);
}

TVMStatus VMFileClose(int filedescriptor)
{
  TVMStatus status;
  callstatus = 0;
  filedescriptor = callreturn;
  MachineFileClose(filedescriptor,closeCallback, NULL);

  while(callstatus == 0){}

  if(callreturn)
    status = VM_STATUS_SUCCESS;

  else
    status = VM_STATUS_FAILURE;

  return status;
}

void readCallback(void * data, int result)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  callstatus = 1;
  returned = result;
  MachineResumeSignals(&signals);
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
{
  filedescriptor = callreturn;
  callstatus = 0;
  MachineFileRead(filedescriptor, data, *length,  readCallback, NULL);
  
  while(callstatus == 0){ /* Waiting */ }

  *length = returned;
  return VM_STATUS_SUCCESS;
}

void writeCallback(void *calldata, int result)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  // cerr << "callback" << endl;
  TVMThreadID* runningID = (TVMThreadID*) calldata;
  // cerr << *runningID << endl;
  threadMap[*runningID] -> state = VM_THREAD_STATE_READY;
  sort(*runningID);
  MachineResumeSignals(&signals);
}

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);

  //filedescriptor= callreturn;
  size_t len = size_t(*length);
  callstatus = 0;

  TVMThreadID runningID;
  VMThreadID(&runningID);

  MachineFileWrite(filedescriptor, data, len, writeCallback, (void*) &runningID);
  // while (callstatus == 0) {}
  VMThreadScheduler(VM_THREAD_STATE_WAITING);

  MachineResumeSignals(&signals);
  //write(filedescriptor, data, len);
  return VM_STATUS_SUCCESS;
}

void seekCallback(void *data, int result)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);

  callstatus = 1;
  callbackreturn = result;
  MachineResumeSignals(&signals);
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
{
  callstatus = 0;
  filedescriptor = callreturn;
  MachineFileSeek(filedescriptor, offset, whence, seekCallback, NULL);
  while(callstatus == 0){}
  *newoffset = callbackreturn;
  
  if(callbackreturn)
    return VM_STATUS_SUCCESS;
  else
    return VM_STATUS_FAILURE;
}

void sleepCallback(void* calldata)  // Decrement all individual sleep counters here
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  
  TVMThreadID running;
  VMThreadID(&running);
  totalticks++;

  if (anythingReady())
  {
    VMThreadScheduler(VM_THREAD_STATE_READY);
  }

  for (unsigned int i = 0; i < threadMap.size(); i++)
  {
    if (threadMap[i] -> sleepCount == 0 && i != 1 && threadMap[i] -> state == VM_THREAD_STATE_WAITING)
    {
      threadMap[i] -> state = VM_THREAD_STATE_READY;
      sort(i);
    }
    else
    {
      (threadMap[i] -> sleepCount)--;
    }
  }
  MachineResumeSignals(&signals);
  return;
}

void createMainThread()
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  TCB* controlblock = new TCB;
  SMachineContextRef machine = new SMachineContext;
  controlblock -> machineContext = *machine;
  controlblock -> priority = VM_THREAD_PRIORITY_NORMAL;
  controlblock -> state = VM_THREAD_STATE_RUNNING;
  controlblock -> stacksize = 0x10000;
  controlblock -> param = (void*) 0;
  controlblock -> stackaddr = NULL;
  controlblock -> entry = NULL;
  controlblock -> sleepCount = 0;

  pair<TVMThreadID, TCB*> currentpair;
  currentpair.first = 0;
  currentpair.second = controlblock;

  threadMap.insert(currentpair);
  MachineResumeSignals(&signals);
  return;
}

void idleSkeleton(void* param)
{
  MachineEnableSignals();

  while(1 > 0)
  {
    if (anythingReady())
    {
      VMThreadScheduler(VM_THREAD_STATE_DEAD);
    }
  }

  return;
}

void createIdleThread()
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  TVMThreadID thread = 1;

  VMThreadCreate(idleSkeleton, NULL, 0x10000, VM_THREAD_PRIORITY_LOW, &thread);
  // VMThreadActivate(1);
  MachineContextCreate(&(threadMap[thread] -> machineContext), idleSkeleton, (void*) (threadMap[thread]), 
                       threadMap[thread] -> stackaddr, threadMap[thread] -> stacksize);
  MachineResumeSignals(&signals);
  return;
}

void MachineInitialize(void); 

TVMMainEntry VMLoadModule(const char *module); 
        
TVMStatus VMStart(int tickms, int argc, char *argv[])
{
  ticks = tickms;
  TVMStatus status;
  MachineInitialize();
  MachineRequestAlarm(tickms*1000, sleepCallback, NULL);
  createMainThread();
  createIdleThread();
  TVMMainEntry loaded = VMLoadModule(argv[0]);

  if (loaded != NULL)
  {    
    loaded(argc, argv);
    status = VM_STATUS_SUCCESS;
  }

  else
    status = VM_STATUS_FAILURE;

  return status;
}

TVMMainEntry VMLoadModule(const char *module); // Implemented in VirtualMachineUtils.c

void VMUnloadModule(void); // Implemented in VirtualMachineUtils.c

TVMStatus VMTickMS(int *tickmsref)
{
  if (tickmsref == NULL)
    return VM_STATUS_ERROR_INVALID_PARAMETER;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  *tickmsref = ticks;
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMTickCount(TVMTickRef tickref)
{
  if (tickref == NULL)
    return VM_STATUS_ERROR_INVALID_PARAMETER;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  *tickref = totalticks;
  // cerr << "Total ticks: " << totalticks << endl;
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid)
{
  if (entry == NULL || tid == NULL)
    return VM_STATUS_ERROR_INVALID_PARAMETER;
  
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  TCB* current = new TCB;
  SMachineContextRef machine = new SMachineContext;
  current -> machineContext = *machine;
  current -> entry = entry;
  current -> param = param;
  current -> stacksize = memsize;
  char *threadstack = new char[memsize];
  current -> stackaddr = (void*) threadstack;
  current -> priority = prio;
  current -> state = VM_THREAD_STATE_DEAD;
  current -> sleepCount = 0;
  *tid = threadMap.size();

  pair<TVMThreadID, TCB*> currentpair;
  currentpair.first = *tid;
  currentpair.second = current;

  threadMap.insert(currentpair);
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadDelete(TVMThreadID thread)
{
  if (thread >= threadMap.size())
    return VM_STATUS_ERROR_INVALID_ID;

  if (threadMap[thread] -> state != VM_THREAD_STATE_DEAD)
    return VM_STATUS_ERROR_INVALID_STATE;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  threadMap.erase(thread);
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

void VMThreadActivateSkeleton(void* param)
{
  MachineEnableSignals();
  TCB* id = (TCB*) param;
  id -> entry(id -> param);
  
  TVMThreadID runningID;
  VMThreadID(&runningID);
  VMThreadTerminate(runningID);
  return;
}

TVMStatus VMThreadActivate(TVMThreadID thread)
{
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  threadMap[thread] -> state = VM_THREAD_STATE_READY;
  TVMThreadID runningID;
  VMThreadID(&runningID);

  MachineContextCreate(&(threadMap[thread] -> machineContext), VMThreadActivateSkeleton, (void*) (threadMap[thread]), 
                       threadMap[thread] -> stackaddr, threadMap[thread] -> stacksize);

  sort(thread);
  
  if (threadMap[runningID] -> priority < threadMap[thread] -> priority)
    VMThreadScheduler(VM_THREAD_STATE_READY);
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadTerminate(TVMThreadID thread)
{
  unsigned int size = threadMap.size();

  if (thread >= size)
    return VM_STATUS_ERROR_INVALID_ID;

  if (threadMap[thread] -> state == VM_THREAD_STATE_DEAD)
    return VM_STATUS_ERROR_INVALID_STATE;
  
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  for (unsigned int i = 0; i < mutexMap.size(); i++)
  {
    if (mutexMap[i] -> lock == true && mutexMap[i] -> owner == thread)
      VMMutexRelease(i);
  }
  
  VMThreadScheduler(VM_THREAD_STATE_DEAD);
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadID(TVMThreadIDRef threadref)
{
  if (threadref == NULL)
    return VM_STATUS_ERROR_INVALID_PARAMETER;

  TVMThreadID i;

  for (i = 0; i < threadMap.size(); i++)
    if (threadMap[i] -> state == VM_THREAD_STATE_RUNNING)
      break;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  *threadref = i;
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref)
{
  if (thread >= threadMap.size())
    return VM_STATUS_ERROR_INVALID_ID;

  if (stateref == NULL)
    return VM_STATUS_ERROR_INVALID_PARAMETER;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  *stateref = threadMap[thread] -> state;
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadSleep(TVMTick tick)
{
  if (tick == VM_TIMEOUT_INFINITE)
    return VM_STATUS_ERROR_INVALID_PARAMETER;

  // if (tick == VM_TIMEOUT_IMMEDIATE)
    

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  // Add a while loop for waiting while counter > 0
  // Return VM_STATUS_SUCCESS if successful

  TVMThreadID id;
  VMThreadID(&id);
  threadMap[id] -> sleepCount = tick;

  VMThreadScheduler(VM_THREAD_STATE_WAITING);

  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
{
  if (mutexref == NULL)
    return VM_STATUS_ERROR_INVALID_PARAMETER;
  
  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  Mutex* mutex = new Mutex;
  mutex -> lock = false;
  mutex -> owner = 0;

  *mutexref = mutexMap.size();

  pair<TVMMutexID, Mutex*> mutexpair;
  mutexpair.first = *mutexref;
  mutexpair.second = mutex;

  mutexMap.insert(mutexpair);
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexDelete(TVMMutexID mutex)
{
  if (mutex >= mutexMap.size())
    return VM_STATUS_ERROR_INVALID_ID;

  if (mutexMap[mutex] -> lock == true)
    return VM_STATUS_ERROR_INVALID_STATE;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  mutexMap.erase(mutex);
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
{
  if (ownerref == NULL)
    return VM_STATUS_ERROR_INVALID_PARAMETER;

  if (mutex >= mutexMap.size())
    return VM_STATUS_ERROR_INVALID_ID;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  if (mutexMap[mutex] -> lock == false)
  {
    *ownerref = VM_THREAD_ID_INVALID;
    return VM_STATUS_SUCCESS;
  }

  *ownerref = mutexMap[mutex] -> owner;
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
{
  if (mutex >= mutexMap.size())
    return VM_STATUS_ERROR_INVALID_ID;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);
  TVMThreadID runningID;
  VMThreadID(&runningID);

  if (timeout == VM_TIMEOUT_IMMEDIATE)
  {
    if (mutexMap[mutex] -> lock == false)
    {
      mutexMap[mutex] -> owner = runningID;
      mutexMap[mutex] -> lock = true;
      MachineResumeSignals(&signals);
      return VM_STATUS_SUCCESS;
    }

    else
    {
      MachineResumeSignals(&signals);
      return VM_STATUS_FAILURE;
    }
  }

  else if (timeout == VM_TIMEOUT_INFINITE)
  {
    if (mutexMap[mutex] -> lock == false)
    {
      mutexMap[mutex] -> owner = runningID;
      mutexMap[mutex] -> lock = true;
      MachineResumeSignals(&signals);
      return VM_STATUS_SUCCESS;
    }
    
    else
    {
      mutexSort(runningID, mutex);
      MachineResumeSignals(&signals);
      VMThreadScheduler(VM_THREAD_STATE_WAITING);
    }
  }

  else
  {
    // Implement with timeout as a tick value
  }

  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexRelease(TVMMutexID mutex)
{
  if (mutex >= mutexMap.size())
    return VM_STATUS_ERROR_INVALID_ID;

  if (mutexMap[mutex] -> lock == false)
    return VM_STATUS_ERROR_INVALID_STATE;

  TMachineSignalState signals;
  MachineSuspendSignals(&signals);

  mutexMap[mutex] -> lock = false;
  mutexMap[mutex] -> owner = 0;

  VMMutexScheduler(mutex);
  MachineResumeSignals(&signals);
  return VM_STATUS_SUCCESS;
}

}
