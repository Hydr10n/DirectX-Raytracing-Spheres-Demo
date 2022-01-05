#include "MyAppData.h"

#include "filesystem"

decltype(MyAppData::Settings::m_appData) MyAppData::Settings::m_appData(std::filesystem::path(*__wargv).replace_filename("Settings.ini").c_str());
