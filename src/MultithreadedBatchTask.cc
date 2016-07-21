#include "MultithreadedBatchTask.hh"
#include "ProgressMonitor.hh"

using namespace std;
using namespace boost;

void
MultithreadedBatchTask::MonitorThread::notify()
{
    {
        unique_lock<mutex> lock(mMutex);
        mThreadSignalled = true;
     }
     mCondVar.notify_one();
}
 

void
MultithreadedBatchTask::MonitorThread::operator()()
{
    if (mThreads.empty())
    {
        return;
    }

    thread_group grp;
    for (uint64_t i = 0; i < mThreads.size(); ++i)
    {
        WorkThread* thr = mThreads[i].get();
        grp.create_thread(boost::bind(&WorkThread::run, thr));
    }

    bool everyoneFinished;
    bool abortAllThreads = false;
    bool waitingForThreadsToAbort = false;
    do
    {
        everyoneFinished = true;
        uint64_t progress = 0ull;

        {
            unique_lock<mutex> lock(mMutex);
            while (!mThreadSignalled)
            {
                mCondVar.wait(lock);
            }
            mThreadSignalled = false;

            for (uint64_t i = 0; i < mThreads.size(); ++i)
            {
                WorkThread* thr = mThreads[i].get();
                unique_lock<mutex> lock(thr->mReportDataMutex);
                everyoneFinished &= thr->mDone;
                progress += thr->mWorkDone;
                abortAllThreads |= thr->mDone &&
                    thr->mReturnState == WorkThread::kExceptionThrown;
            }

            if (abortAllThreads && !waitingForThreadsToAbort)
            {
                waitingForThreadsToAbort = true;

                // One of the threads exited abnormally, so tell the
                // rest of them to stop.
                for (uint64_t i = 0; i < mThreads.size(); ++i)
                {
                    WorkThread* thr = mThreads[i].get();
                    unique_lock<mutex> lock(thr->mReportDataMutex);
                    if (!thr->mDone)
                    {
                        thr->mAbortRequested = true;
                    }
                }
            }
        }

        mProgressMon.tick(progress);
    }  while (!everyoneFinished);

    grp.join_all();

    // If a thread threw an exception, rethrow it here.  If several
    // did, we rethrow only the first.
    //
    // TODO - It would be good if we could throw a composite exception.
    for (uint64_t i = 0; i < mThreads.size(); ++i)
    {
        WorkThread* thr = mThreads[i].get();
        switch (thr->mReturnState)
        {
            case WorkThread::kExceptionThrown:
            {
                rethrow_exception(thr->mThrownException);
                break;
            }
            case WorkThread::kAborted:
            case WorkThread::kNormalReturn:
            {
                break;
            }
        }
    }
    mProgressMon.end();
}


MultithreadedBatchTask::MonitorThread::MonitorThread(
        ProgressMonitorBase& pProgressMon)
    : mProgressMon(pProgressMon), mThreadSignalled(false)
{
}


void
MultithreadedBatchTask::WorkThread::run()
{
    try {
        operator()();

        {
            unique_lock<mutex> lock(mReportDataMutex);
            mDone = true;
        }
    }
    catch (...)
    {
        unique_lock<mutex> lock(mReportDataMutex);
        mDone = true;
        mThrownException = boost::current_exception();
        mReturnState = kExceptionThrown;
    }
    mMonThread.notify();
}

bool
MultithreadedBatchTask::WorkThread::reportWorkDone(uint64_t pWorkDone)
{
    {
        unique_lock<mutex> lock(mReportDataMutex);
        mWorkDone = pWorkDone;
        if (mAbortRequested)
        {
            mDone = true;
            mReturnState = kAborted;
            lock.unlock();
            mMonThread.notify();
            return false;
        }
    }

    mMonThread.notify();
    return true;
}


MultithreadedBatchTask::WorkThread::WorkThread(MultithreadedBatchTask& pTask)
    : mWorkDone(0), mDone(false),
      mAbortRequested(false), mMonThread(pTask.mMonThread),
      mReturnState(kNormalReturn), mThrownException()
{
}

