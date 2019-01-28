#include "stdafx.h"
#include <list>
#include <mutex>
#include "RecCountLimiter.h"
#include <Psapi.h>
#include <Pdh.h>
#pragma comment(lib, "pdh.lib")

namespace
{
	template<class T>
	class BufList
	{
	public:
		void pushBack(T t)
		{
			std::lock_guard<std::mutex> guard(mLock);
			mList.push_back(t);
		}
		bool popFront(T t)
		{
			std::lock_guard<std::mutex> guard(mLock);
			T first = mList.front();
			if (first == t)
			{
				mList.pop_front();
				return true;
			}
			return false;
		}
		int size()
		{
			std::lock_guard<std::mutex> guard(mLock);
			return mList.size();
		}
	private:
		std::list<T> mList;
		std::mutex mLock;
	};

	static int gAvailableThreadCount;
	static int gTotalThreadCount;
	static std::mutex gLockAvailableThread;
	static BufList<int> gWaitBuf;

	static ULARGE_INTEGER gLastCPU, gLastSysCPU, gLastUserCPU;
	static int gNumProcessors;
	static HANDLE gCurrentProcess;
	static PDH_HQUERY gCpuQuery;
	static PDH_HCOUNTER gCpuTotal;

	void init() 
	{
		SYSTEM_INFO sysInfo;
		FILETIME ftime, fsys, fuser;

		GetSystemInfo(&sysInfo);
		gNumProcessors = sysInfo.dwNumberOfProcessors;

		GetSystemTimeAsFileTime(&ftime);
		memcpy(&gLastCPU, &ftime, sizeof(FILETIME));

		gCurrentProcess = GetCurrentProcess();
		GetProcessTimes(gCurrentProcess, &ftime, &ftime, &fsys, &fuser);
		memcpy(&gLastSysCPU, &fsys, sizeof(FILETIME));
		memcpy(&gLastUserCPU, &fuser, sizeof(FILETIME));
		//为避免错误，最少设置两个线程同时运行
		if (gNumProcessors < 2)
		{
			gNumProcessors = 2;
		}
		gAvailableThreadCount = gTotalThreadCount = gNumProcessors;
	}

	void pdhinit() 
	{
		PdhOpenQuery(NULL, NULL, &gCpuQuery);
		// You can also use L"\\Processor(*)\\% Processor Time" and get individual CPU values with PdhGetFormattedCounterArray()
		PdhAddEnglishCounter(gCpuQuery, L"\\Processor(_Total)\\% Processor Time", NULL, &gCpuTotal);
		PdhCollectQueryData(gCpuQuery);
	}

	//启动时初始化
	class Init { public:Init() { init(); pdhinit(); } }it;
	//当前等待的序号
	int GetIndex()
	{
		static std::mutex mutex;
		static int start = 0;
		std::lock_guard<std::mutex> guard(mutex);
		return ++start;
	}
	//最小线程数，根据实际情况通过增减
	inline int MinimumThread()
	{
		return gNumProcessors - 1;
	}
	//最大线程数，根据实际情况增减
	inline int MaxmumThread()
	{
		return gNumProcessors * 3;
	}
	//删减线程数量的阈值,此处为CPU有一个核心空闲，根据实际情况增减
	inline double DelThreadThreshold()
	{
		return double(gNumProcessors - 1) / gNumProcessors*100;//%
	}

	void MakeFullUseOfCPU()
	{
		static std::mutex mutex;
		std::lock_guard<std::mutex> guard(mutex);
		//如果当前CPU利用率过低，则增加线程数，如果CPU利用率过高，则降低
		//线程数最低为CPU核心数，最多为CPU核心数的2倍
		double globalCpuUsage = ThreadControl::GetGlobalCPUUsage();
		//只有当没有可用线程时，才需要增加线程数
		if (gAvailableThreadCount <= 0)
		{
			if (globalCpuUsage < DelThreadThreshold() && gTotalThreadCount < MaxmumThread())
			{
				ThreadControl::SetLimitCount(gTotalThreadCount + 1);
			}
		}
		else
		{
			if (globalCpuUsage > DelThreadThreshold() && gTotalThreadCount > MinimumThread())
			{
				ThreadControl::SetLimitCount(gTotalThreadCount - 1);
			}
		}
	}
}

namespace ThreadControl
{
	//获取可用链接，如果没有则一直等待
	void WaitForAvailableRec(int index)
	{
		//动态增减线程数，充分利用CPU
		MakeFullUseOfCPU();
		//加入当前等待的序号
		gWaitBuf.pushBack(index);
		//block,until there is an available one
		while (true)
		{
			{
				std::lock_guard<std::mutex> guard(gLockAvailableThread);
				//如果有可用线程，而且轮到当前等待的序号，则执行，否则继续等待
				if (gAvailableThreadCount >= 1 && gWaitBuf.popFront(index))
				{
					gAvailableThreadCount -= 1;
					break;
				}
			}
			::Sleep(50);
		}
	}

	//释放可用链接
	void ReleaseAvailableRec()
	{
		std::lock_guard<std::mutex> guard(gLockAvailableThread);
		gAvailableThreadCount += 1;
	}

	double GetCPUUsage()
	{
		FILETIME ftime, fsys, fuser;
		ULARGE_INTEGER now, sys, user;
		double percent;

		GetSystemTimeAsFileTime(&ftime);
		memcpy(&now, &ftime, sizeof(FILETIME));

		GetProcessTimes(gCurrentProcess, &ftime, &ftime, &fsys, &fuser);
		memcpy(&sys, &fsys, sizeof(FILETIME));
		memcpy(&user, &fuser, sizeof(FILETIME));
		percent = double((sys.QuadPart - gLastSysCPU.QuadPart) + (user.QuadPart - gLastUserCPU.QuadPart));
		percent /= (now.QuadPart - gLastCPU.QuadPart);
		percent /= gNumProcessors;
		gLastCPU = now;
		gLastUserCPU = user;
		gLastSysCPU = sys;

		return percent * 100;
	}

	double GetGlobalCPUUsage()
	{
		PDH_FMT_COUNTERVALUE counterVal;

		PdhCollectQueryData(gCpuQuery);
		PdhGetFormattedCounterValue(gCpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal);
		return counterVal.doubleValue;
	}

	SIZE_T GetMemoryUsage()
	{
		PROCESS_MEMORY_COUNTERS pmc;
		GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
		//Physical Memory currently used by current process : in bytes.
		SIZE_T physMemUsedByMe = pmc.WorkingSetSize;
		return physMemUsedByMe;
	}

	void SetLimitCount(unsigned int count)
	{
		int diff = count - gTotalThreadCount;
		gTotalThreadCount = count;
		std::lock_guard<std::mutex> guard(gLockAvailableThread);
		gAvailableThreadCount += diff;
	}

	int GetCpuCoreCount()
	{
		return gNumProcessors;
	}

	ScopeThreadLimiter::ScopeThreadLimiter()
	{
		WaitForAvailableRec(GetIndex());
	}

	ScopeThreadLimiter::~ScopeThreadLimiter()
	{
		ReleaseAvailableRec();
	}
}

