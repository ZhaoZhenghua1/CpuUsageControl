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
		//Ϊ��������������������߳�ͬʱ����
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

	//����ʱ��ʼ��
	class Init { public:Init() { init(); pdhinit(); } }it;
	//��ǰ�ȴ������
	int GetIndex()
	{
		static std::mutex mutex;
		static int start = 0;
		std::lock_guard<std::mutex> guard(mutex);
		return ++start;
	}
	//��С�߳���������ʵ�����ͨ������
	inline int MinimumThread()
	{
		return gNumProcessors - 1;
	}
	//����߳���������ʵ���������
	inline int MaxmumThread()
	{
		return gNumProcessors * 3;
	}
	//ɾ���߳���������ֵ,�˴�ΪCPU��һ�����Ŀ��У�����ʵ���������
	inline double DelThreadThreshold()
	{
		return double(gNumProcessors - 1) / gNumProcessors*100;//%
	}

	void MakeFullUseOfCPU()
	{
		static std::mutex mutex;
		std::lock_guard<std::mutex> guard(mutex);
		//�����ǰCPU�����ʹ��ͣ��������߳��������CPU�����ʹ��ߣ��򽵵�
		//�߳������ΪCPU�����������ΪCPU��������2��
		double globalCpuUsage = ThreadControl::GetGlobalCPUUsage();
		//ֻ�е�û�п����߳�ʱ������Ҫ�����߳���
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
	//��ȡ�������ӣ����û����һֱ�ȴ�
	void WaitForAvailableRec(int index)
	{
		//��̬�����߳������������CPU
		MakeFullUseOfCPU();
		//���뵱ǰ�ȴ������
		gWaitBuf.pushBack(index);
		//block,until there is an available one
		while (true)
		{
			{
				std::lock_guard<std::mutex> guard(gLockAvailableThread);
				//����п����̣߳������ֵ���ǰ�ȴ�����ţ���ִ�У���������ȴ�
				if (gAvailableThreadCount >= 1 && gWaitBuf.popFront(index))
				{
					gAvailableThreadCount -= 1;
					break;
				}
			}
			::Sleep(50);
		}
	}

	//�ͷſ�������
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

