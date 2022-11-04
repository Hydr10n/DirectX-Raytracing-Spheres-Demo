#pragma once

template <typename T, uint N>
struct Stack {
	static const uint MaxCount = N;
	uint Count;
	T Data[N];

	void Initialize() { Count = 0; }

	bool IsEmpty() { return !Count; }

	T GetTop() { return Data[Count - 1]; }

	void SetTop(T element) { Data[Count - 1] = element; }

	void Push(T element) { Data[Count++] = element; }

	void Pop() { Count--; }
};
