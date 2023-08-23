module;

#include <set>

export module SharedData;

import DisplayHelpers;

using namespace DisplayHelpers;
using namespace std;

export namespace SharedData {
	Resolution g_displayResolution;
	set<Resolution> g_displayResolutions;
}
