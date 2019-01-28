#pragma once
#include <Windows.h>

namespace ThreadControl
{
	//������������
	class ScopeThreadLimiter
	{
	public:
		ScopeThreadLimiter();
		~ScopeThreadLimiter();
	};
	//��ǰ�����CPU������
	double GetCPUUsage();
	//���Ե�����CPU������
	double GetGlobalCPUUsage();
	//��ǰ������ڴ�ʹ����
	SIZE_T GetMemoryUsage();
	//�����߳���������
	void SetLimitCount(unsigned int count);
	//��ȡCPU������
	int GetCpuCoreCount();
}

