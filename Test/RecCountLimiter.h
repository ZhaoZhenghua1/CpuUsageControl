#pragma once
#include <Windows.h>

namespace ThreadControl
{
	//连接数量控制
	class ScopeThreadLimiter
	{
	public:
		ScopeThreadLimiter();
		~ScopeThreadLimiter();
	};
	//当前程序的CPU利用率
	double GetCPUUsage();
	//电脑的整体CPU利用率
	double GetGlobalCPUUsage();
	//当前程序的内存使用量
	SIZE_T GetMemoryUsage();
	//设置线程限制数量
	void SetLimitCount(unsigned int count);
	//获取CPU核心数
	int GetCpuCoreCount();
}

