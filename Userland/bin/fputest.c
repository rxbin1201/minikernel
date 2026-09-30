#include "libc.h"

/* fputest [k]: rechnet mit double und float, gibt zwischendurch die CPU ab (sys_yield). Laeuft ein zweites fputest
 * mit anderem k gleichzeitig, wuerden ohne Sichern der FPU-Register beim Threadwechsel dessen Werte hier landen.
 * Ergebnis 0 = beide Rechnungen gleich (mit und ohne Abgeben), 1 = Abweichung. */
void _start(int argc, char **argv)
{
    int k = argc > 1 ? atoi(argv[1]) : 1;
    double kd = k, ref = 0, sum = 0;
    float kf = (float)k, fref = 0, fsum = 0;
    for (int i = 1; i <= 100000; i++) {
        ref += kd / ((double)i * i + kd);
        fref += kf / ((float)i + kf);
    }
    for (int i = 1; i <= 100000; i++) {
        sum += kd / ((double)i * i + kd);
        fsum += kf / ((float)i + kf);
        if (i % 1000 == 0)
            sys_yield();
    }
    int ok = sum == ref && fsum == fref;
    printf("fputest %d: %s (%lld / %lld)\n", k, ok ? "ok" : "FEHLER", (long long)(sum * 1e9), (long long)(fsum * 1e3));
    sys_exit(ok ? 0 : 1);
}
