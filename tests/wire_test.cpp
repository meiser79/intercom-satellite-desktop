// Tokens as Kiosk Satellite and the Android app make them: signed, for one call, used once.
#include "../src/Wire.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    qint64 now = 1'000'000;
    Wire a("shared key", [&] { return now; }), b("shared key", [&] { return now; }),
        other("other key", [&] { return now; });

    QString token = a.token("call-1", "kiosk-a");
    check(b.verify(token, "call-1").value("from").toString() == "kiosk-a", "valid token accepted");
    check(b.verify(token, "call-1").isEmpty(), "a token is used once");
    check(other.verify(a.token("call-1", "kiosk-a"), "call-1").isEmpty(), "wrong key refused");
    check(b.verify(a.token("call-1", "kiosk-a"), "call-2").isEmpty(), "other call refused");

    QString late = a.token("call-1", "kiosk-a");
    now += 60000;
    check(b.verify(late, "call-1").isEmpty(), "expired token refused");

    check(Wire::fingerprint("test") == "9f86d081", "fingerprint");
    check(Wire::validCallId("abc-DEF_123") && !Wire::validCallId("a/b") && !Wire::validCallId(""),
          "call ids");

    // A token made by the Python simulator, the same scheme as the Android app.
    Wire fixed("test", [] { return qint64(1'700'000'000'000); });
    QString python =
        "eyJpbnRlcmNvbSI6ICJ4IiwgImZyb20iOiAieSIsICJuIjogIjEiLCAiZXhwIjogMTcwMDAwMDA1MDAwMH0=."
        "oIfY8CV4DO2rzSL3jJ5cbYESIsWrDwE0jEhyCnKw_4E=";
    check(fixed.verify(python, "x").value("from").toString() == "y", "interoperable token");

    if (failures == 0) std::printf("wire: all checks passed\n");
    return failures ? 1 : 0;
}
