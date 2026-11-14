// Custom Crosshair "Pattern" grid: 17 comma-separated hex rows, bit x = column x.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
inline const std::string crosshairDefault="0,0,100,100,100,100,100,0,7c7c,0,100,100,100,100,100,0,0";   // 17 rows, bit x = column x
inline std::vector<uint32_t> CrosshairRows(const std::string& text){std::vector<uint32_t> rows;size_t from=0;while(from<=text.size()&&rows.size()<17){size_t to=text.find(',',from);if(to==std::string::npos)to=text.size();rows.push_back((uint32_t)strtoul(text.substr(from,to-from).c_str(),nullptr,16)&0x1ffff);from=to+1;}while(rows.size()<17)rows.push_back(0);return rows;}
inline std::string CrosshairText(const std::vector<uint32_t>& rows){std::string out;char b[12];for(size_t i=0;i<rows.size();++i){snprintf(b,sizeof(b),"%x",rows[i]);if(i)out+=",";out+=b;}return out;}
