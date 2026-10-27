/*
 * MonitorModel 压力自测: 模拟"依次勾绘制 -> 再勾结构体监控 -> 反复增删"流程,
 * 检测悬垂/重复释放导致的堆损坏。
 */
#include "../app/core/monitor_model.h"
#include <QCoreApplication>
#include <QDebug>
#include <cstdio>

namespace {
int fails = 0;
void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

DwarfVar leafVar(const char* name, uint32_t addr) {
    DwarfVar v;
    v.name = name; v.addr = addr; v.size = 4;
    v.type = VariableDef::Float;
    v.isStruct = false;
    return v;
}

DwarfVar structVehicle() {
    DwarfVar v;
    v.name = "sim.vehicle"; v.addr = 0x20000000; v.size = 12;
    v.isStruct = true;
    v.members.append(leafVar("speed", 0x20000000));
    v.members.append(leafVar("current", 0x20000004));
    v.members.append(leafVar("temp", 0x20000008));
    return v;
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    for (int round = 0; round < 200; ++round) {
        MonitorModel m;
        QObject::connect(&m, &MonitorModel::changed, [&m] {
            (void)m.rootCount();   /* 访问结构, 触发潜在问题 */
        });
        /* 依次勾绘制 -> 三个叶子根 */
        m.addDwarfRoot(leafVar("sim.vehicle.speed", 0x20000000));
        m.addDwarfRoot(leafVar("sim.vehicle.current", 0x20000004));
        m.addDwarfRoot(leafVar("sim.vehicle.temp", 0x20000008));
        m.updateLeafValue(0, 1.0);
        m.updateLeafValue(1, 2.0);
        m.updateLeafValue(2, 3.0);
        /* 再勾结构体监控 */
        m.addDwarfRoot(structVehicle());
        /* 移除前三个单叶根 */
        m.removeRoot(0); m.removeRoot(0); m.removeRoot(0);
        /* 再增删 */
        m.addDwarfRoot(leafVar("sim.position", 0x2000000C));
        m.removeRoot(m.rootCount() - 1);
        if (m.leafCount() != 3) { expect(false, "leafCount==3"); break; }
    }
    expect(true, "200 轮压力循环无崩溃");

    /* 单独验证移除后下标重映射 */
    {
        MonitorModel m;
        m.addDwarfRoot(structVehicle());
        m.addDwarfRoot(leafVar("sim.position", 0x2000000C));
        const int posLi = m.leafIndexByPath("sim.position");
        expect(posLi == 3, "position leaf idx==3");
        m.removeRoot(0);   /* 移除结构体 */
        const int posLi2 = m.leafIndexByPath("sim.position");
        expect(posLi2 == 0, "重映射后 position idx==0");
        m.updateLeafValue(posLi2, 9.0);
        expect(m.leaf(posLi2).cur == 9.0, "重映射后叶子值可写");
    }

    std::printf(fails ? "== FAILED %d ==\n" : "== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
