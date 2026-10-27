#include "gui/mainwindow.h"
#include <QApplication>
#include <QSettings>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
/* 崩溃日志: 把异常地址与所在模块写入 crash.log, 便于 addr2line 定位 */
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("crash.log", "w");
    if (f) {
        const uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        fprintf(f, "code=0x%08lX addr=0x%llX\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode,
                (unsigned long long)addr);
        /* 尝试定位所属模块基址 */
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
            char mod[MAX_PATH] = {};
            GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, MAX_PATH);
            fprintf(f, "module=%s base=0x%llX offset=0x%llX\n", mod,
                    (unsigned long long)(uintptr_t)mbi.AllocationBase,
                    (unsigned long long)(addr - (uintptr_t)mbi.AllocationBase));
        }
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif
    QApplication app(argc, argv);
    app.setOrganizationName("GS32");
    app.setApplicationName("GS32Scope");
    app.setApplicationVersion("0.1.0");
    QSettings::setDefaultFormat(QSettings::IniFormat);   /* 会话持久化存 ini(便于查看/迁移) */

    MainWindow w;
    w.show();
    return app.exec();
}
