// thx https://github.com/jbro129/Unity-Substrate-Hook-Android

#include <jni.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "utils.h"
#include <android/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <iostream>
#include <fstream>

using namespace std;

long long baseAddr(const char *soname)  // credits to https://github.com/ikoz/AndroidSubstrate_hookingC_examples/blob/master/nativeHook3/jni/nativeHook3.cy.cpp
{
    void *imagehandle = dlopen(soname, RTLD_LOCAL | RTLD_LAZY);
    if (soname == NULL)
        return (long long)NULL;
    if (imagehandle == NULL){
        return (long long)NULL;
    }
    uintptr_t * irc = NULL;
    FILE *f = NULL;
    char line[200] = {0};
    char *state = NULL;
    char *tok = NULL;
    char * baseAddr = NULL;
    if ((f = fopen("/proc/self/maps", "r")) == NULL)
        return (long long)NULL;
    while (fgets(line, 199, f) != NULL)
    {
        tok = strtok_r(line, "-", &state);
        baseAddr = tok;
        tok = strtok_r(NULL, "\t ", &state);
        tok = strtok_r(NULL, "\t ", &state); // "r-xp" field
        tok = strtok_r(NULL, "\t ", &state); // "0000000" field
        tok = strtok_r(NULL, "\t ", &state); // "01:02" field
        tok = strtok_r(NULL, "\t ", &state); // "133224" field
        tok = strtok_r(NULL, "\t ", &state); // path field

        if (tok != NULL) {
            int i;
            for (i = (int)strlen(tok)-1; i >= 0; --i) {
                if (!(tok[i] == ' ' || tok[i] == '\r' || tok[i] == '\n' || tok[i] == '\t'))
                    break;
                tok[i] = 0;
            }
            {
                size_t toklen = strlen(tok);
                size_t solen = strlen(soname);
                if (toklen > 0) {
                    if (toklen >= solen && strcmp(tok + (toklen - solen), soname) == 0) {
                        fclose(f);
                        return (long long)strtoll(baseAddr,NULL,16);
                    }
                }
            }
        }
    }
    fclose(f);
    return (long long)NULL;
}

long long location; // save lib.so base address so we do not have to recalculate every time causing lag.

long long getRealOffset(void* offset) // calculate dump.cs address + lib.so base address.
{
    if (location == 0)
    {
        //arm
        location = baseAddr(IL2CPP_SO_PATH); // replace the com.package.name with the package name of the app you are modding.
    }
    return location + (long long)offset;
}

long long FindPattern(long long dwAddress, const char* pattern, long long dwSearchRangeLen) {
	#define in_range(x, a, b) (x >= a && x <= b)
	#define get_bits(x) (in_range((x & (~0x20)), 'A', 'F') ? ((x & (~0x20)) - 'A' + 0xA): (in_range(x, '0', '9') ? x - '0': 0))
	#define get_byte(x) (get_bits(x[0]) << 4 | get_bits(x[1]))

    // To avoid a lot of bad match candidates, pre-process wildcards at the front of the pattern
    long long skippedStartBytes = 0;
    while(pattern[0] == '\?') {
        // see comments below for insight on these numbers
        pattern += (pattern[1] == '\?') ? 3 : 2;
        skippedStartBytes++;
    }
    long long match = 0;  // current match candidate
	const char* pat = pattern;  // current spot in the pattern

    // TODO: align dwAddress to word boundary first, then iterate by 4?
	for (long long pCur = dwAddress + skippedStartBytes; pCur < dwAddress + dwSearchRangeLen; pCur++) {
		if (!pat[0]) break;  // end of pattern means match is complete!
		if (pat[0] == '\?' || *(char *)pCur == get_byte(pat)) {  // does this pCur match this pat?
			if (!match) match = pCur;  // start match
			if (!pat[2]) break;  // no more chars in pattern means match is complete!

			if (pat[0] != '\?' || pat[1] == '\?') {
				pat += 3;  // advance past "xy " or "?? "
            } else {
				pat += 2;  // advance past "? "
            }
		} else {
            if (match) pCur = match;  // reset search position to beginning of the failed match; for loop will begin new search at match + 1
			pat = pattern;
			match = 0;
		}
	}
	return match ? match - skippedStartBytes : 0;
}

long long FindUniquePattern(bool& multiple, long long dwAddress, const char* pattern, const char* label, long long dwSearchRangeLen) {
    long long firstMatchAddr = 0, newMatchAddr, start = dwAddress, dwEnd = dwAddress + dwSearchRangeLen;
    int matches = 0;
    while (start > 0 && start < dwEnd && (newMatchAddr = FindPattern(start, pattern, dwEnd - start))) {
        if (!firstMatchAddr) firstMatchAddr = newMatchAddr;
        matches++;
        if (label) log(DEBUG, "Sigscan found possible \"%s\": offset 0x%llX, pointer 0x%llX", label, newMatchAddr - dwAddress, newMatchAddr);
        start = newMatchAddr + 1;
        log(DEBUG, "start = %llX, end = %llX", start, dwEnd);
        usleep(1000);
    }
    if (matches > 1) {
        multiple = true;
        log(WARNING, "Multiple sig scan matches for \"%s\"!", label);
    }
    return firstMatchAddr;
}

// BEAT SABER SPECIFIC

// il2cpp_string_new, used to find string construction offset: 0x2DD144
// il2cpp_string_new immediate call offset: 0x30A1C8
// Creation of string method(char* chars, size_t length): 0x30A1E8

void setcsstr(Il2CppString* in, u16string_view str) {
    in->length = str.length();
    for(int i = 0; i < in->length; i++) {
        // Can assume that each char is only a single char (a single word --> double word)
        in->chars[i] = str[i];
    }
}

// Inspired by DaNike
string to_utf8(u16string_view view) {
    char dat[view.length() + 1];
    transform(view.data(), view.data() + view.size(), dat, [](auto utf16_char) {
        return static_cast<char>(utf16_char);
    });
    dat[view.length()] = '\0';
    return {dat};
}

u16string to_utf16(string_view view) {
    char16_t dat[view.length() + 1];
    transform(view.data(), view.data() + view.size(), dat, [](auto standardChar) {
        return static_cast<char16_t>(standardChar);
    });
    dat[view.length()] = '\0';
    return {dat};
}

u16string_view csstrtostr(Il2CppString* in)
{
    return {in->chars, static_cast<uint32_t>(in->length)};
}

// Thanks DaNike!
void dump(int before, int after, void* ptr) {
    log(DEBUG, "Dumping Immediate Pointer: %p: %08x", ptr, *reinterpret_cast<int*>(ptr));
    auto begin = static_cast<int*>(ptr) - before;
    auto end = static_cast<int*>(ptr) + after;
    for (auto cur = begin; cur != end; ++cur) {
        log(DEBUG, "%p: %08x", cur, *cur);
    }
}

// BEAT SABER SETTINGS

bool fileexists(const char* filename) {
    return access(filename, W_OK | R_OK) != -1;
}

bool direxists(const char* dirname) {
    struct stat info;

    if (stat(dirname, &info) != 0) {
        return false;
    } if (info.st_mode & S_IFDIR) {
        return true;
    }
    return false;
}

char* readfile(const char* filename) {
    FILE* fp = fopen(filename, "r");
    char* content = NULL;
    long size = 0;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        rewind(fp);
        content = (char*)malloc(size * sizeof(char));
        fread(content, sizeof(char), size, fp);
        fclose(fp);
    }
    return content;
}

bool writefile(const char* filename, const char* text) {
    FILE* fp = fopen(filename, "w");
    if (fp) {
        fwrite(text, sizeof(char), strlen(text), fp);
        fclose(fp);
        return true;
    }
    return false;
}

bool deletefile(const char* filename) {
    if (fileexists(filename))
        return remove(filename) == 0;
    return false;
}

void* loadfromasset(const char* assetFilePath, const char* assetName) {
    // TODO IMPLEMENT
    // Create C# string
    auto str = il2cpp_utils::createcsstr(assetFilePath);
    void* fromFileParams[] = {str};
    // auto asyncBundle = il2cpp_functions::runtime_invoke()
    return nullptr;
}

bool parsejson(ConfigDocument& doc, string_view js) {
    char temp[js.length()];
    memcpy(temp, js.data(), js.length());
    
    if (doc.ParseInsitu(temp).HasParseError()) {
        return false;
    }
    return true;
}

string getconfigpath() {
    string filename;
    filename = filename.append(CONFIG_PATH);
    filename = filename.append(MOD_ID);
    filename = filename.append(".json");
    return filename;
}

