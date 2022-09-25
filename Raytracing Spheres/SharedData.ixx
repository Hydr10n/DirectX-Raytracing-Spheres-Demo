module;

#include <set>

export module SharedData;

import DisplayHelpers;

using namespace DisplayHelpers;
using namespace std;

export {
	inline set<Resolution> g_displayResolutions;
}
