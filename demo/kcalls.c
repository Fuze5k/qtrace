/*
 * kcalls: make kernel calls that <sys/kercalls.h> has but the SAT event table
 * does not document, so a trace shows qtrace naming them:
 *
 *   qtrace ./kcalls
 *
 * MsgReceivePulse() gets a real pulse; the interrupt and pipe calls get
 * invalid ids on purpose and fail, which still enters the kernel.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/neutrino.h>

static void show(const char *what, int r)
{
    printf("%-26s = %d%s%s\n", what, r, r == -1 ? " " : "", r == -1 ? strerror(errno) : "");
}

int main(void)
{
    struct _pulse p;
    int chid, coid;

    chid = ChannelCreate(0);
    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (chid == -1 || coid == -1) { perror("kcalls: channel"); return 1; }
    show("MsgSendPulse(code 1, 42)", MsgSendPulse(coid, -1, 1, 42));
    show("MsgReceivePulse()", MsgReceivePulse(chid, &p, sizeof p, NULL));
    printf("%-26s   code %d, value %d\n", "", p.code, p.value.sival_int);

    show("InterruptUnblock(-1)", InterruptUnblock(-1));
    show("InterruptCtl(-1)", InterruptCtl(-1, 0, NULL, 0));
    show("PipeOpen(-1, -1)", PipeOpen(-1, -1, NULL, 0));
    show("PipeCtl(-1)", PipeCtl(-1, NULL));
    show("PipeClose(-1)", PipeClose(-1));

    ConnectDetach(coid);
    ChannelDestroy(chid);
    return 0;
}
