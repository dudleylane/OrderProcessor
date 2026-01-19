/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cassert>
#include <cstring>
#include <climits>
#include <thread>
#include <chrono>
#include "ExchUtils.h"

namespace{
	const size_t NUMBERS_AMOUNT = 100;
	const char *NUMBERS[] = {
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", 
		"10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
		"20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
		"30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
		"40", "41", "42", "43", "44", "45", "46", "47", "48", "49",
		"50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
		"60", "61", "62", "63", "64", "65", "66", "67", "68", "69",
		"70", "71", "72", "73", "74", "75", "76", "77", "78", "79",
		"80", "81", "82", "83", "84", "85", "86", "87", "88", "89",
		"90", "91", "92", "93", "94", "95", "96", "97", "98", "99"
/*		,

		"100", "101", "102", "103", "104", "105", "106", "107", "108", "109", 
		"110", "111", "112", "113", "114", "115", "116", "117", "118", "119",
		"120", "121", "122", "123", "124", "125", "126", "127", "128", "129",
		"130", "131", "132", "133", "134", "135", "136", "137", "138", "139",
		"140", "141", "142", "143", "144", "145", "146", "147", "148", "149",
		"150", "151", "152", "153", "154", "155", "156", "157", "158", "159",
		"160", "161", "162", "163", "164", "165", "166", "167", "168", "169",
		"170", "171", "172", "173", "174", "175", "176", "177", "178", "179",
		"180", "181", "182", "183", "184", "185", "186", "187", "188", "189",
		"190", "191", "192", "193", "194", "195", "196", "197", "198", "199",	

		"200", "201", "202", "203", "204", "205", "206", "207", "208", "209", 
		"210", "211", "212", "213", "214", "215", "216", "217", "218", "219",
		"220", "221", "222", "223", "224", "225", "226", "227", "228", "229",
		"230", "231", "232", "233", "234", "235", "236", "237", "238", "239",
		"240", "241", "242", "243", "244", "245", "246", "247", "248", "249",
		"250", "251", "252", "253", "254", "255", "256", "257", "258", "259",
		"260", "261", "262", "263", "264", "265", "266", "267", "268", "269",
		"270", "271", "272", "273", "274", "275", "276", "277", "278", "279",
		"280", "281", "282", "283", "284", "285", "286", "287", "288", "289",
		"290", "291", "292", "293", "294", "295", "296", "297", "298", "299",	

		"300", "301", "302", "303", "304", "305", "306", "307", "308", "309", 
		"310", "311", "312", "313", "314", "315", "316", "317", "318", "319",
		"320", "321", "322", "323", "324", "325", "326", "327", "328", "329",
		"330", "331", "332", "333", "334", "335", "336", "337", "338", "339",
		"340", "341", "342", "343", "344", "345", "346", "347", "348", "349",
		"350", "351", "352", "353", "354", "355", "356", "357", "358", "359",
		"360", "361", "362", "363", "364", "365", "366", "367", "368", "369",
		"370", "371", "372", "373", "374", "375", "376", "377", "378", "379",
		"380", "381", "382", "383", "384", "385", "386", "387", "388", "389",
		"390", "391", "392", "393", "394", "395", "396", "397", "398", "399",	
*/

	};
	
	char *append(char *buf, size_t val){
		char *p = buf;
		assert(val < NUMBERS_AMOUNT);
		strcat(buf, NUMBERS[val]);
		if(val >= 10)
			++p;
		++p;
		return p;	
	}

	char *append_2PosZeroFwd(char *buf, size_t val){
		char *p = buf;
		assert(val < NUMBERS_AMOUNT);
		if(val < 10)
			strcat(buf, NUMBERS[0]);
		strcat(buf, NUMBERS[val]);
		return p + 2;	
	}

}

namespace aux{

char *toStr(char *buf, size_t val){
	assert(nullptr != buf);
	buf[0] = 0;
	if(val >= NUMBERS_AMOUNT){
		char *p = buf;
		size_t part1 = val%NUMBERS_AMOUNT;
		size_t v = val/NUMBERS_AMOUNT;
		if(v < NUMBERS_AMOUNT)
			p = append(buf, v);
		else{
			size_t part2 = v%NUMBERS_AMOUNT;
			v /= NUMBERS_AMOUNT;
			if(v < NUMBERS_AMOUNT)
				p = append(buf, v);
			else{
				size_t part3 = v%NUMBERS_AMOUNT;
				v /= NUMBERS_AMOUNT;
				if(v < NUMBERS_AMOUNT)
					p = append(buf, v);
				else{
					size_t part4 = v%NUMBERS_AMOUNT;
					v /= NUMBERS_AMOUNT;
					if(v < NUMBERS_AMOUNT)
						p = append(buf, v);
					else{
						size_t part5 = v%NUMBERS_AMOUNT;
						v /= NUMBERS_AMOUNT;
						assert(v < NUMBERS_AMOUNT);
						p = append(buf, v);
						p = append_2PosZeroFwd(p, part5);
					}
					p = append_2PosZeroFwd(p, part4);
				}
				p = append_2PosZeroFwd(p, part3);
			}
			p = append_2PosZeroFwd(p, part2);
		}
		return append_2PosZeroFwd(p, part1);
	}else 
		return append(buf, val);
}

char *toStr(char *buf, int val){
	assert(nullptr != buf);
	if(0 > val){
		buf[0] = '-';
		++buf;
		val *= -1;
	}
	return toStr(buf, static_cast<size_t>(val));
}

char *toStr(char *buf, int val, size_t pos){
	if(0 > val){
		buf[0] = '-';
		++buf;
		val *= -1;
	}
	buf[0] = 0;
	size_t usePos = 0;
	if(val < 10)
		usePos = 1;
	else if(val < 100)
		usePos = 2;
	else if(val < 1000)
		usePos = 3;
	else if(val < 10000)
		usePos = 4;
	else if(val < 10000)
		usePos = 5;
	else if(val < 100000)
		usePos = 6;
	else if(val < 1000000)
		usePos = 7;
	else if(val < 10000000)
		usePos = 8;
	else if(val < 100000000)
		usePos = 9;
	else if(val < 1000000000)
		usePos = 10;
	else 
		usePos = 11;
	if(usePos < pos){
		for(size_t i = usePos; i < pos; ++i)
			buf = append(buf, 0);
	}
	return toStr(buf, val);
}

char *toStr(char *buf, COP::u32 val){
	assert(nullptr != buf);
	return toStr(buf, static_cast<size_t>(val));
}

char *toStr(char *buf, COP::u32 val, size_t pos){
	buf[0] = 0;
	size_t usePos = 0;
	if(val < 10)
		usePos = 1;
	else if(val < 100)
		usePos = 2;
	else if(val < 1000)
		usePos = 3;
	else if(val < 10000)
		usePos = 4;
	else if(val < 100000)
		usePos = 5;
	else if(val < 1000000)
		usePos = 6;
	else if(val < 10000000)
		usePos = 7;
	else if(val < 100000000)
		usePos = 8;
	else if(val < 1000000000)
		usePos = 9;
	else
		usePos = 10;
	if(usePos < pos){
		for(size_t i = usePos; i < pos; ++i)
			buf = append(buf, 0);
	}
	return toStr(buf, static_cast<size_t>(val));
}

// u64 and i64 overloads only if different from size_t (Windows or 32-bit systems)
#if defined(_WIN32) || ULONG_MAX == 0xFFFFFFFF
char *toStr(char *buf, COP::u64 val){
	assert(nullptr != buf);
	buf[0] = 0;
	if(val >= NUMBERS_AMOUNT){
		char *p = buf;
		size_t part1 = static_cast<size_t>(val%NUMBERS_AMOUNT);
		COP::u64 v = val/NUMBERS_AMOUNT;
		if(v < NUMBERS_AMOUNT)
			p = append(buf, static_cast<size_t>(v));
		else{
			size_t part2 = static_cast<size_t>(v%NUMBERS_AMOUNT);
			v /= NUMBERS_AMOUNT;
			if(v < NUMBERS_AMOUNT)
				p = append(buf, static_cast<size_t>(v));
			else{
				size_t part3 = static_cast<size_t>(v%NUMBERS_AMOUNT);
				v /= NUMBERS_AMOUNT;
				if(v < NUMBERS_AMOUNT)
					p = append(buf, static_cast<size_t>(v));
				else{
					size_t part4 = static_cast<size_t>(v%NUMBERS_AMOUNT);
					v /= NUMBERS_AMOUNT;
					if(v < NUMBERS_AMOUNT)
						p = append(buf, static_cast<size_t>(v));
					else{
						size_t part5 = static_cast<size_t>(v%NUMBERS_AMOUNT);
						v /= NUMBERS_AMOUNT;
						if(v < NUMBERS_AMOUNT)
							p = append(buf, static_cast<size_t>(v));
						else{
							size_t part6 = static_cast<size_t>(v%NUMBERS_AMOUNT);
							v /= NUMBERS_AMOUNT;
							if(v < NUMBERS_AMOUNT)
								p = append(buf, static_cast<size_t>(v));
							else{
								size_t part7 = static_cast<size_t>(v%NUMBERS_AMOUNT);
								v /= NUMBERS_AMOUNT;
								if(v < NUMBERS_AMOUNT)
									p = append(buf, static_cast<size_t>(v));
								else{
									size_t part8 = static_cast<size_t>(v%NUMBERS_AMOUNT);
									v /= NUMBERS_AMOUNT;
									if(v < NUMBERS_AMOUNT)
										p = append(buf, static_cast<size_t>(v));
									else{
										size_t part9 = static_cast<size_t>(v%NUMBERS_AMOUNT);
										v /= NUMBERS_AMOUNT;
										if(v < NUMBERS_AMOUNT)
											p = append(buf, static_cast<size_t>(v));
										else{
											size_t part10 = static_cast<size_t>(v%NUMBERS_AMOUNT);
											v /= NUMBERS_AMOUNT;
											assert(v < NUMBERS_AMOUNT);
											p = append(buf, static_cast<size_t>(v));
											p = append_2PosZeroFwd(p, part10);
										}
										p = append_2PosZeroFwd(p, part9);
									}
									p = append_2PosZeroFwd(p, part8);
								}
								p = append_2PosZeroFwd(p, part7);
							}
							p = append_2PosZeroFwd(p, part6);
						}
						p = append_2PosZeroFwd(p, part5);
					}
					p = append_2PosZeroFwd(p, part4);
				}
				p = append_2PosZeroFwd(p, part3);
			}
			p = append_2PosZeroFwd(p, part2);
		}
		return append_2PosZeroFwd(p, part1);
	}else 
		return append(buf, static_cast<size_t>(val));
}

char *toStr(char *buf, COP::i64 val){
	assert(nullptr != buf);
	if(0 > val){
		buf[0] = '-';
		++buf;
		val *= -1;
	}
	return toStr(buf, static_cast<COP::u64>(val));
}
#endif // _WIN32 || 32-bit

COP::DateTimeT currentDateTime()
{
	return 0;
}

void WaitInterval(int mseconds)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(mseconds));
}

}