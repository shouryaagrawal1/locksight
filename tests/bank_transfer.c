/*
 * bank_transfer.c - a realistic deadlock.
 *
 * transfer(from, to) locks the "from" account first, then the "to" account.
 * That looks harmless, but if one thread runs transfer(a, b) while another
 * runs transfer(b, a), the lock order is reversed: a classic real-world bug.
 */
#include "common.h"

typedef struct {
    const char     *name;
    int             balance;
    pthread_mutex_t lock;
} Account;

static Account acc_a = { "alice", 1000, PTHREAD_MUTEX_INITIALIZER };
static Account acc_b = { "bob",   1000, PTHREAD_MUTEX_INITIALIZER };

static void transfer(Account *from, Account *to, int amount)
{
    pthread_mutex_lock(&from->lock);
    sleep_ms(g_lucky ? 0 : 200);        /* models slow work (e.g. a fraud check) */
    pthread_mutex_lock(&to->lock);

    from->balance -= amount;
    to->balance   += amount;
    printf("[bank] moved %d from %s to %s\n", amount, from->name, to->name);

    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

static void *alice_pays_bob(void *arg)
{
    (void)arg;
    transfer(&acc_a, &acc_b, 100);
    return NULL;
}

static void *bob_pays_alice(void *arg)
{
    (void)arg;
    transfer(&acc_b, &acc_a, 50);
    return NULL;
}

int main(int argc, char **argv)
{
    parse_mode(argc, argv);
    start_watchdog(5);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, alice_pays_bob, NULL);
    if (g_lucky) pthread_join(t1, NULL);
    pthread_create(&t2, NULL, bob_pays_alice, NULL);
    if (!g_lucky) pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("[bank] final balances: alice=%d bob=%d\n", acc_a.balance, acc_b.balance);
    finished("bank_transfer");
    return 0;
}
