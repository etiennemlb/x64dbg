/**
@file symbolinfo.cpp

@brief Implements the symbolinfo class.
*/

#include "symbolinfo.h"
#include "debugger.h"
#include "console.h"
#include "module.h"
#include "addrinfo.h"
#include "dbghelp_safe.h"

struct SYMBOLCBDATA
{
    CBSYMBOLENUM cbSymbolEnum;
    void* user;
    std::vector<char> decoratedSymbol;
    std::vector<char> undecoratedSymbol;
};

static void SymEnumImports(duint Base, CBSYMBOLENUM EnumCallback, SYMBOLCBDATA & cbData)
{
    SYMBOLINFO symbol;
    memset(&symbol, 0, sizeof(SYMBOLINFO));
    symbol.isImported = true;
    apienumimports(Base, [&](duint base, duint addr, char* name, char* moduleName)
    {
        cbData.decoratedSymbol[0] = '\0';
        cbData.undecoratedSymbol[0] = '\0';

        symbol.addr = addr;
        symbol.decoratedSymbol = cbData.decoratedSymbol.data();
        symbol.undecoratedSymbol = cbData.undecoratedSymbol.data();
        strncpy_s(symbol.decoratedSymbol, MAX_SYM_NAME, name, _TRUNCATE);

        // Convert a mangled/decorated C++ name to a readable format
        if(!SafeUnDecorateSymbolName(name, symbol.undecoratedSymbol, MAX_SYM_NAME, UNDNAME_COMPLETE))
            symbol.undecoratedSymbol = nullptr;
        else if(!strcmp(symbol.decoratedSymbol, symbol.undecoratedSymbol))
            symbol.undecoratedSymbol = nullptr;

        EnumCallback(&symbol, cbData.user);
    });
}

void SymEnum(duint Base, CBSYMBOLENUM EnumCallback, void* UserData)
{
    SYMBOLCBDATA cbData;
    cbData.cbSymbolEnum = EnumCallback;
    cbData.user = UserData;
    cbData.decoratedSymbol.resize(MAX_SYM_NAME + 1);
    cbData.undecoratedSymbol.resize(MAX_SYM_NAME + 1);

    {
        SHARED_ACQUIRE(LockModules);
        MODINFO* modInfo = ModInfoFromAddr(Base);
        if(modInfo && modInfo->symbols->isOpen())
        {
            modInfo->symbols->enumSymbols([&cbData, Base](const SymbolInfo & info)
            {
                cbData.decoratedSymbol[0] = '\0';
                cbData.undecoratedSymbol[0] = '\0';

                SYMBOLINFO curSymbol;
                memset(&curSymbol, 0, sizeof(SYMBOLINFO));

                curSymbol.addr = Base + info.addr;
                curSymbol.decoratedSymbol = cbData.decoratedSymbol.data();
                curSymbol.undecoratedSymbol = cbData.undecoratedSymbol.data();
                strncpy_s(curSymbol.decoratedSymbol, MAX_SYM_NAME, info.decoratedName.c_str(), _TRUNCATE);
                strncpy_s(curSymbol.undecoratedSymbol, MAX_SYM_NAME, info.undecoratedName.c_str(), _TRUNCATE);

                // Skip bad ordinals
                if(strstr(curSymbol.decoratedSymbol, "Ordinal"))
                {
                    // Does the symbol point to the module base?
                    if(curSymbol.addr == Base)
                        return true;
                }

                // Convert a mangled/decorated C++ name to a readable format
                if(info.decoratedName == info.undecoratedName)
                {
                    curSymbol.decoratedSymbol = curSymbol.undecoratedSymbol;
                    curSymbol.undecoratedSymbol = nullptr;
                }

                // Mark IAT entries as Imports
                curSymbol.isImported = strncmp(curSymbol.decoratedSymbol, "__imp_", 6) == 0;

                cbData.cbSymbolEnum(&curSymbol, cbData.user);
                return true;
            });
        }
    }

    // Emit pseudo entry point symbol
    SYMBOLINFO symbol;
    memset(&symbol, 0, sizeof(SYMBOLINFO));
    symbol.decoratedSymbol = "OptionalHeader.AddressOfEntryPoint";
    symbol.addr = ModEntryFromAddr(Base);
    if(symbol.addr)
        EnumCallback(&symbol, UserData);

    SymEnumImports(Base, EnumCallback, cbData);
}

void SymEnumFromCache(duint Base, CBSYMBOLENUM EnumCallback, void* UserData)
{
    SymEnum(Base, EnumCallback, UserData);
}

bool SymGetModuleList(std::vector<SYMBOLMODULEINFO>* List)
{
    ModEnum([List](const MODINFO & mod)
    {
        SYMBOLMODULEINFO curMod;
        curMod.base = mod.base;
        strcpy_s(curMod.name, mod.name);
        strcat_s(curMod.name, mod.extension);
        List->push_back(curMod);
    });
    return true;
}

void SymUpdateModuleList()
{
    // Build the vector of modules
    std::vector<SYMBOLMODULEINFO> modList;

    if(!SymGetModuleList(&modList))
    {
        GuiSymbolUpdateModuleList(0, nullptr);
        return;
    }

    // Create a new array to be sent to the GUI thread
    size_t moduleCount = modList.size();
    SYMBOLMODULEINFO* data = (SYMBOLMODULEINFO*)BridgeAlloc(moduleCount * sizeof(SYMBOLMODULEINFO));

    // Direct copy from std::vector data
    memcpy(data, modList.data(), moduleCount * sizeof(SYMBOLMODULEINFO));

    // Send the module data to the GUI for updating
    GuiSymbolUpdateModuleList((int)moduleCount, data);
}

void SymDownloadAllSymbols(const char* SymbolStore)
{
    // Default to Microsoft's symbol server
    if(!SymbolStore)
        SymbolStore = "https://msdl.microsoft.com/download/symbols";

    // Build the vector of modules
    std::vector<SYMBOLMODULEINFO> modList;

    if(!SymGetModuleList(&modList))
        return;

    // Skip loading if there aren't any found modules
    if(modList.empty())
        return;

    // Backup the current symbol search path
    wchar_t oldSearchPath[MAX_PATH];

    if(!SafeSymGetSearchPathW(fdProcessInfo->hProcess, oldSearchPath, MAX_PATH))
    {
        dputs(QT_TRANSLATE_NOOP("DBG", "SymGetSearchPathW failed!"));
        return;
    }

    // Use the custom server path and directory
    char customSearchPath[MAX_PATH * 2];
    sprintf_s(customSearchPath, "SRV*%s*%s", szSymbolCachePath, SymbolStore);

    auto symOptions = SafeSymGetOptions();
    SafeSymSetOptions(symOptions & ~SYMOPT_IGNORE_CVREC);

    const WString search_paths[] =
    {
        WString(),
        StringUtils::Utf8ToUtf16(customSearchPath)
    };

    // Reload
    for(auto & module : modList)
    {
        for(unsigned k = 0; k < sizeof(search_paths) / sizeof(*search_paths); k++)
        {
            const WString & cur_path = search_paths[k];

            if(!SafeSymSetSearchPathW(fdProcessInfo->hProcess, cur_path.c_str()))
            {
                dputs(QT_TRANSLATE_NOOP("DBG", "SymSetSearchPathW (1) failed!"));
                continue;
            }

            dprintf(QT_TRANSLATE_NOOP("DBG", "Downloading symbols for %s...\n"), module.name);

            wchar_t modulePath[MAX_PATH];
            if(!GetModuleFileNameExW(fdProcessInfo->hProcess, (HMODULE)module.base, modulePath, MAX_PATH))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "GetModuleFileNameExW (%p) failed!\n"), module.base);
                continue;
            }

            if(!SafeSymUnloadModule64(fdProcessInfo->hProcess, (DWORD64)module.base))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "SymUnloadModule64 (%p) failed!\n"), module.base);
                continue;
            }

            if(!SafeSymLoadModuleExW(fdProcessInfo->hProcess, 0, modulePath, 0, (DWORD64)module.base, 0, 0, 0))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "SymLoadModuleEx (%p) failed!\n"), module.base);
                continue;
            }

            // symbols are lazy-loaded so let's load them and get the real return value

            IMAGEHLP_MODULEW64 info;
            info.SizeOfStruct = sizeof(info);

            if(!SafeSymGetModuleInfoW64(fdProcessInfo->hProcess, (DWORD64)module.base, &info))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "SymGetModuleInfo64 (%p) failed!\n"), module.base);
                continue;
            }

            bool status;

            switch(info.SymType)
            {
            // XXX there may be more enum values meaning proper load
            case SymPdb:
                status = 1;
                break;
            default:
            case SymExport: // always treat export symbols as failure
                status = 0;
                break;
            }

            if(status)
                break;
        }
    }

    SafeSymSetOptions(symOptions);

    // Restore the old search path
    if(!SafeSymSetSearchPathW(fdProcessInfo->hProcess, oldSearchPath))
        dputs(QT_TRANSLATE_NOOP("DBG", "SymSetSearchPathW (2) failed!"));
}

bool SymAddrFromName(const char* Name, duint* Address)
{
    if(!Name || Name[0] == '\0')
        return false;

    if(!Address)
        return false;

    // Skip 'OrdinalXXX'
    if(!_strnicmp(Name, "Ordinal", 7))
        return false;

    // According to MSDN:
    // Note that the total size of the data is the SizeOfStruct + (MaxNameLen - 1) * sizeof(TCHAR)
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];

    PSYMBOL_INFO symbol = (PSYMBOL_INFO)&buffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_LABEL_SIZE;

    if(!SafeSymFromName(fdProcessInfo->hProcess, Name, symbol))
        return false;

    *Address = (duint)symbol->Address;
    return true;
}

String SymGetSymbolicName(duint Address)
{
    //
    // This resolves an address to a module and symbol:
    // [modname.]symbolname
    //
    char label[MAX_SYM_NAME];
    char modname[MAX_MODULE_SIZE];
    auto hasModule = ModNameFromAddr(Address, modname, false);

    // User labels have priority, but if one wasn't found,
    // default to a symbol lookup
    if(!DbgGetLabelAt(Address, SEG_DEFAULT, label))
    {
        if(hasModule)
            return StringUtils::sprintf("%s.%p", modname, Address);
        return "";
    }

    if(hasModule)
        return StringUtils::sprintf("<%s.%s>", modname, label);
    return StringUtils::sprintf("<%s>", label);
}

bool SymGetSourceLine(duint Cip, char* FileName, int* Line, DWORD* disp)
{
    MODINFO* modInfo = ModInfoFromAddr(Cip);
    if(!modInfo)
        return false;

    SymbolSourceBase* sym = modInfo->symbols;
    if(!sym || sym == &EmptySymbolSource)
        return false;

    LineInfo lineInfo;
    if(!sym->findSourceLineInfo(Cip - modInfo->base, lineInfo))
        return false;

    if(disp)
        *disp = 0;

    if(Line)
        *Line = lineInfo.lineNumber;

    if(FileName)
        strcpy_s(FileName, MAX_STRING_SIZE, lineInfo.sourceFile.c_str());

    return true;
}
