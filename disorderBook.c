/*  Crazy attempt to write the disorderBook backend in C.
    The data layout stolen from DanielVF.

    We store all data in memory so that the user can retrieve it later.
    As such, there are very few free() calls.


    PROTOCOL:

    We don't handle user input directly. The frontend is responsible for
    sending us commands as single lines. Only the ORDER command is tricky:

    ORDER  <account>  <account_id>  <(int32) qty>  <(int32) price>  <dir:1|2>  <orderType:1|2|3|4>

    e.g.

    ORDER  CES134127       5             100             5000           1               3

    The frontend must give each account a unique, low, non-negative integer as
    an id (RAM is allocated based on these, so keep them as low as possible).

    Numbers for direction and orderType are defined below.

    Other commands:

    QUOTE
    ORDERBOOK
    CANCEL <id>
    STATUS <id>
    STATUSALL <account_id>

    __SCORES__
    __DEBUG_MEMORY__
    __ACC_FROM_ID__ <id>

    This last is not a direct response to a user query, but can be used by the
    frontend for authentication purposes (i.e. is the user entitled to cancel
    this order?)

    */

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// On Windows, need this so we can use _setmode so we don't send \r\n
#if defined(_WIN32)
    #include <fcntl.h>
#endif

#define BUY 1       // Don't change these now, they are also used in the frontend
#define SELL 2

#define LIMIT 1     // Don't change these now, they are also used in the frontend
#define MARKET 2
#define FOK 3
#define IOC 4

#define MAXSTRING 2048
#define SMALLSTRING 64
#define MAXTOKENS 64                // Well-behaved frontend will never send this many

#define MAXORDERS 2000000000        // Not going all the way to MAX_INT, because various numbers might go above this
#define MAXACCOUNTS 5000

#define TOO_MANY_ORDERS 1
#define SILLY_VALUE 2
#define TOO_HIGH_ACCOUNT 3


#define EXECUTION_TEMPLATE_1 "{\n\
  \"ok\": true,\n\
  \"account\": \"%s\",\n\
  \"venue\": \"%s\",\n\
  \"symbol\": \"%s\",\n\
  \"order\":\n"

#define EXECUTION_TEMPLATE_2 ",\n\
  \"standingId\": %d,\n\
  \"incomingId\": %d,\n\
  \"price\": %d,\n\
  \"filled\": %d,\n\
  \"filledAt\": \"%s\",\n\
  \"standingComplete\": %s,\n\
  \"incomingComplete\": %s\n\
}"

#define INDENT_2 "  "
#define INDENT_4 "    "

typedef struct Fill_struct {
    int price;
    int qty;
    char * ts;
} FILL;

typedef struct FillNode_struct {
    struct Fill_struct * fill;
    struct FillNode_struct * prev;
    struct FillNode_struct * next;
} FILLNODE;

typedef struct Account_struct {
    char name[SMALLSTRING];
    struct Order_struct ** orders;
    int arraylen;
    int count;
    int posmin;
    int posmax;
    int shares;
    int cents;
} ACCOUNT;

typedef struct Order_struct {
    int direction;
    int originalQty;
    int qty;
    int price;
    int orderType;
    int id;
    struct Account_struct * account;
    char * ts;
    struct FillNode_struct * firstfillnode;
    int totalFilled;
    int open;
} ORDER;

typedef struct OrderNode_struct {
    struct Order_struct * order;
    struct OrderNode_struct * prev;
    struct OrderNode_struct * next;
} ORDERNODE;

typedef struct Level_struct {
    struct Level_struct * prev;
    struct Level_struct * next;
    int price;
    struct OrderNode_struct * firstordernode;
} LEVEL;

typedef struct OrderPtrAndError_struct {
    struct Order_struct * order;
    int error;
} ORDER_AND_ERROR;

typedef struct Quote_struct {       // Anything that can exceed 2^31-1 is an int64
    int64_t bidSize;
    int64_t askSize;
    int64_t bidDepth;
    int64_t askDepth;
    int bid;
    int ask;
    int last;
    int lastSize;
    char lastTrade[SMALLSTRING];
    char quoteTime[SMALLSTRING];
} QUOTE;

typedef struct DebugInfo_struct {
    int inits_of_level;
    int inits_of_fill;
    int inits_of_fillnode;
    int inits_of_order;
    int inits_of_ordernode;
    int inits_of_account;

    int reallocs_of_global_order_list;
    int reallocs_of_global_account_list;
    int reallocs_of_account_order_list;
} DEBUG_INFO;


// ---------------------------------- GLOBALS -----------------------------------------------


char Venue[SMALLSTRING];
char Symbol[SMALLSTRING];
char * StartTime = NULL;

LEVEL * FirstBidLevel = NULL;
LEVEL * FirstAskLevel = NULL;

ORDER ** AllOrders = NULL;
int CurrentOrderArrayLen = 0;
int HighestKnownOrder = -1;

ACCOUNT ** AllAccounts = NULL;      // The array of all accounts gets realloc'd as needed,
int CurrentAccountArrayLen = 0;     // but it should probably simply have a fixed size.

QUOTE Quote = {0, 0, 0, 0, -1, -1, -1, -1, "", ""};

DEBUG_INFO DebugInfo = {0};         // Think global is auto-zeroed anyway, but whatever


// ------------------------------------------------------------------------------------------


void end_message (FILE * outfile)
{
    fprintf(outfile, "\nEND\n");
    fflush(outfile);
    return;
}


void check_ptr_or_quit (void * ptr)
{
    if (ptr == NULL)
    {
        printf("{\"ok\": false, \"error\": \"Out of memory! Quitting\"}");
        end_message(stdout);
        assert(ptr);
    }
    return;
}


LEVEL * init_level (int price, ORDERNODE * ordernode, LEVEL * prev, LEVEL * next)
{
    LEVEL * ret;

    DebugInfo.inits_of_level++;

    ret = malloc(sizeof(LEVEL));
    check_ptr_or_quit(ret);

    ret->price = price;
    ret->firstordernode = ordernode;
    ret->prev = prev;
    ret->next = next;

    return ret;
}


FILL * init_fill (int price, int qty, char * ts)
{
    FILL * ret;

    DebugInfo.inits_of_fill++;

    ret = malloc(sizeof(FILL));
    check_ptr_or_quit(ret);

    ret->price = price;
    ret->qty = qty;
    ret->ts = ts;

    return ret;
}


FILLNODE * init_fillnode (FILL * fill, FILLNODE * prev, FILLNODE * next)
{
    FILLNODE * ret;

    DebugInfo.inits_of_fillnode++;

    ret = malloc(sizeof(FILLNODE));
    check_ptr_or_quit(ret);

    ret->fill = fill;
    ret->prev = prev;
    ret->next = next;

    return ret;
}


ORDERNODE * init_ordernode (ORDER * order, ORDERNODE * prev, ORDERNODE * next)
{
    ORDERNODE * ret;

    DebugInfo.inits_of_ordernode++;

    ret = malloc(sizeof(ORDERNODE));
    check_ptr_or_quit(ret);

    ret->order = order;
    ret->prev = prev;
    ret->next = next;

    return ret;
}


int next_id (int no_iterate_flag)
{
    static int id = 0;

    if (id == MAXORDERS)         // Stop iterating
    {
        return MAXORDERS;
    } else {
        if (no_iterate_flag)
        {
            return id;
        } else {
            return id++;
        }
    }
}


// Various safer strcpy functions exist. I thought of dumping strlcpy() into the file but this
// creature of my own devising does the same thing, minus the return value (which I don't need).

void safe_strcpy (char * dest, char * source, size_t size)
{
    size_t n;

    if (size == 0) return;              // size_t is unsigned, 0 is lowest possible
    for (n = 0; n < size - 1; n++)
    {
        dest[n] = source[n];
        if (dest[n] == '\0') return;    // Copied whole string
    }
    dest[size - 1] = '\0';              // Truncated (or copied all if size was exactly right)
    return;
}


char * new_timestamp (void)
{
    char * timestamp;
    time_t t;
    struct tm * ti;
    static struct tm last_time = {0};
    static int fake_micro = 0;

    timestamp = malloc(SMALLSTRING);
    check_ptr_or_quit(timestamp);

    t = time(NULL);

    if (t != (time_t) -1)
    {
        ti = gmtime(&t);
    } else {
        ti = NULL;
    }

    if (ti)
    {
        // We fake microseconds by using the number of times the
        // function has been called this second as "microseconds",
        // so first check if second has rolled over...
        if (last_time.tm_year == ti->tm_year &&
            last_time.tm_mon  == ti->tm_mon  &&
            last_time.tm_mday == ti->tm_mday &&
            last_time.tm_hour == ti->tm_hour &&
            last_time.tm_min  == ti->tm_min  &&
            last_time.tm_sec  == ti->tm_sec)
        {
            fake_micro += 1;
        } else {
            fake_micro = 0;
            last_time = *ti;
        }
        snprintf(timestamp, SMALLSTRING, "%d-%02d-%02dT%02d:%02d:%02d.%06dZ",
                 ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, ti->tm_hour, ti->tm_min, ti->tm_sec, fake_micro);
    } else {
        snprintf(timestamp, SMALLSTRING, "Unknown");
    }

    return timestamp;
}


ORDER * init_order (ACCOUNT * account, int qty, int price, int direction, int orderType, int id)
{
    ORDER * ret;
    int n;

    DebugInfo.inits_of_order++;

    ret = malloc(sizeof(ORDER));
    check_ptr_or_quit(ret);

    ret->direction = direction;
    ret->originalQty = qty;
    ret->qty = qty;
    ret->price = price;
    ret->orderType = orderType;
    ret->id = id;
    ret->account = account;
    ret->ts = new_timestamp();
    ret->firstfillnode = NULL;
    ret->totalFilled = 0;
    ret->open = 1;

    // Now deal with the global order storage...

    while (id >= CurrentOrderArrayLen)
    {
        AllOrders = realloc(AllOrders, (CurrentOrderArrayLen + 8192) * sizeof(ORDER *));
        check_ptr_or_quit(AllOrders);
        CurrentOrderArrayLen += 8192;

        // NULLify the new pointers in case gaps open up somehow - we can
        // verify the order ID doesn't exist... (should be impossible at
        // time of writing but it's good practice).

        for (n = CurrentOrderArrayLen - 8192; n < CurrentOrderArrayLen; n++)
        {
            AllOrders[n] = NULL;
        }

        DebugInfo.reallocs_of_global_order_list++;
    }
    AllOrders[id] = ret;
    HighestKnownOrder = id;

    return ret;
}


ORDER_AND_ERROR * init_o_and_e (void)
{
    ORDER_AND_ERROR * ret;

    ret = malloc(sizeof(ORDER_AND_ERROR));
    check_ptr_or_quit(ret);

    ret->order = NULL;
    ret->error = 0;

    return ret;
}


void update_account (ACCOUNT * account, int quantity, int price, int direction)
{
    // Elsewhere we rather rely on shares and cents being int32,
    // so don't change that now. If people exceed the bounds,
    // their extra shares and money is "lost". Fine.

    int64_t tmp64;

    assert(account);

    // Update shares...

    tmp64 = account->shares;

    if (direction == BUY)
    {
        tmp64 += quantity;
        if (tmp64 > 2147483647)
        {
            account->shares = 2147483647;
        } else {
            account->shares = (int) tmp64;
        }
    } else {
        tmp64 -= quantity;
        if (tmp64 < -2147483647)
        {
            account->shares = -2147483647;
        } else {
            account->shares = (int) tmp64;
        }
    }

    // Update cents...

    tmp64 = account->cents;

    if (direction == BUY)
    {
        tmp64 -= (int64_t) price * (int64_t) quantity;
        if (tmp64 < -2147483647)
        {
            account->cents = -2147483647;
        } else {
            account->cents = (int) tmp64;
        }
    } else {
        tmp64 += (int64_t) price * (int64_t) quantity;
        if (tmp64 > 2147483647)
        {
            account->cents = 2147483647;
        } else {
            account->cents = (int) tmp64;
        }
    }

    if (account->shares < account->posmin) account->posmin = account->shares;
    if (account->shares > account->posmax) account->posmax = account->shares;

    return;
}


int64_t get_size_from_level (LEVEL * level)
{
    ORDERNODE * ordernode;
    int64_t ret;

    if (level == NULL)
    {
        return 0;
    }

    ret = 0;

    for (ordernode = level->firstordernode; ordernode != NULL; ordernode = ordernode->next)
    {
        ret += ordernode->order->qty;
    }
    return ret;
}


int64_t get_depth (LEVEL * level)        // Returns size of this level and all worse levels (if level exists)
{
    int64_t onesize;
    int64_t ret;

    ret = 0;

    for ( ; level != NULL; level = level->next)
    {
        ret += get_size_from_level(level);
    }
    return ret;
}


void print_quote (FILE * outfile)       // Just hard-codes the indent, meaning executions messages look odd. Meh.
{
    char buildup[MAXSTRING];
    char part[MAXSTRING];

    // Add all the fields that are always present...
    snprintf(buildup, MAXSTRING, "{\n  \"ok\": true,\n  \"symbol\": \"%s\",\n  \"venue\": \"%s\",\n  \"bidSize\": %" PRId64 ",\n"
                                 "  \"askSize\": %" PRId64 ",\n  \"bidDepth\": %" PRId64 ",\n  \"askDepth\": %" PRId64 ",\n  \"quoteTime\": \"%s\"",
             Symbol, Venue, Quote.bidSize, Quote.askSize, Quote.bidDepth, Quote.askDepth, Quote.quoteTime);

    if (Quote.bid >= 0)         // -1 used as a null value
    {
        snprintf(part, MAXSTRING, ",\n  \"bid\": %d", Quote.bid);
        strncat(buildup, part, MAXSTRING - strlen(buildup) - 1);
    }

    if (Quote.ask >= 0)         // -1 used as a null value
    {
        snprintf(part, MAXSTRING, ",\n  \"ask\": %d", Quote.ask);
        strncat(buildup, part, MAXSTRING - strlen(buildup) - 1);
    }

    if (Quote.lastTrade[0])     // i.e. check the timestamp of the last trade is a non-empty string
    {
        snprintf(part, MAXSTRING, ",\n  \"lastTrade\": \"%s\",\n  \"lastSize\": %d,\n  \"last\": %d", Quote.lastTrade, Quote.lastSize, Quote.last);
        strncat(buildup, part, MAXSTRING - strlen(buildup) - 1);
    }

    strncat(buildup, "\n}", MAXSTRING - strlen(buildup) - 1);

    fprintf(outfile, "%s", buildup);

    return;
}


void print_fills (FILE * outfile, ORDER * order, char * indent1, char * indent2)
{
    FILLNODE * fillnode;

    if (order->firstfillnode == NULL)   // Can do without this block but it's uglier
    {
        fprintf(outfile, "%s\"fills\": []", indent1);
        return;
    }

    fprintf(outfile, "%s\"fills\": [\n", indent1);

    fillnode = order->firstfillnode;

    while (fillnode != NULL)
    {
        if (fillnode != order->firstfillnode) fprintf(outfile, ",\n");
        fprintf(outfile, "%s{\"price\": %d, \"qty\": %d, \"ts\": \"%s\"}", indent2, fillnode->fill->price, fillnode->fill->qty, fillnode->fill->ts);
        fillnode = fillnode->next;
    }

    fprintf(outfile, "\n%s]", indent1);
    return;
}


void print_order (FILE * outfile, ORDER * order)
{
    char orderType_to_print[SMALLSTRING];

    if (order->orderType == LIMIT)
    {
        safe_strcpy(orderType_to_print, "limit", SMALLSTRING);
    } else if (order->orderType == MARKET) {
        safe_strcpy(orderType_to_print, "market", SMALLSTRING);
    } else if (order->orderType == IOC) {
        safe_strcpy(orderType_to_print, "immediate-or-cancel", SMALLSTRING);
    } else if (order->orderType == FOK) {
        safe_strcpy(orderType_to_print, "fill-or-kill", SMALLSTRING);
    } else {
        safe_strcpy(orderType_to_print, "unknown", SMALLSTRING);
    }

    fprintf(outfile,

            "{\n  \"ok\": true,\n  \"venue\": \"%s\",\n  \"symbol\": \"%s\",\n  \"direction\": \"%s\",\n  \"originalQty\": %d,\n  \"qty\": %d,"
            "\n  \"price\": %d,\n  \"orderType\": \"%s\",\n  \"id\": %d,\n  \"account\": \"%s\",\n  \"ts\": \"%s\",\n  \"totalFilled\": %d,\n  \"open\": %s,\n",

            Venue, Symbol, order->direction == BUY ? "buy" : "sell", order->originalQty, order->qty,
            order->price, orderType_to_print, order->id, order->account->name, order->ts, order->totalFilled, order->open ? "true" : "false");

    print_fills(outfile, order, INDENT_2, INDENT_4);
    fprintf(outfile, "\n}");

    return;
}


void create_ticker_message (void)
{
    fprintf(stderr, "TICKER %s %s %s\n", "NONE", Venue, Symbol);

    fprintf(stderr, "{\"ok\": true, \"quote\": ");
    print_quote(stderr);
    fprintf(stderr, "}");

    end_message(stderr);
    return;
}


void create_execution_messages(ORDER * standing, ORDER * incoming, int quantity, int price, char * ts)
{
    fprintf(stderr, "EXECUTION %s %s %s\n", standing->account->name, Venue, Symbol);
    fprintf(stderr, EXECUTION_TEMPLATE_1, standing->account->name, Venue, Symbol);
    print_order(stderr, standing);
    fprintf(stderr, EXECUTION_TEMPLATE_2, standing->id, incoming->id, price, quantity, ts,
            standing->open ? "false" : "true", incoming->open ? "false" : "true");

    end_message(stderr);

    fprintf(stderr, "EXECUTION %s %s %s\n", incoming->account->name, Venue, Symbol);
    fprintf(stderr, EXECUTION_TEMPLATE_1, incoming->account->name, Venue, Symbol);
    print_order(stderr, incoming);
    fprintf(stderr, EXECUTION_TEMPLATE_2, standing->id, incoming->id, price, quantity, ts,
            standing->open ? "false" : "true", incoming->open ? "false" : "true");

    end_message(stderr);
    return;
}


// The following function remakes the parts of the quote that are
// determined by the state of the book itself (i.e. NOT "last trade" info)

void remake_most_of_quote (void)
{
    char * ts;

    Quote.bidSize = get_size_from_level(FirstBidLevel);
    Quote.bidDepth = get_depth(FirstBidLevel);
    Quote.askSize = get_size_from_level(FirstAskLevel);
    Quote.askDepth = get_depth(FirstAskLevel);

    if (FirstBidLevel)
    {
        Quote.bid = FirstBidLevel->price;
    } else {
        Quote.bid = -1;
    }

    if (FirstAskLevel)
    {
        Quote.ask = FirstAskLevel->price;
    } else {
        Quote.ask = -1;
    }

    ts = new_timestamp();
    safe_strcpy(Quote.quoteTime, ts, SMALLSTRING);
    free(ts);

    // We can't touch last, lastSize, or lastTrade
    // as this function is called often even when
    // no actual fill has happened.

    return;
}


void set_quote_lastinfo (int last, int lastSize)
{
    char * ts;

    Quote.last = last;
    Quote.lastSize = lastSize;

    ts = new_timestamp();
    safe_strcpy(Quote.lastTrade, ts, SMALLSTRING);
    free(ts);

    return;
}


void cross (ORDER * standing, ORDER * incoming)
{
    int quantity;
    int price;
    char * ts;
    FILLNODE * currentfillnode;
    FILL * fill;

    ts = new_timestamp();

    if (standing->qty < incoming->qty)
    {
        quantity = standing->qty;
    } else {
        quantity = incoming->qty;
    }

    standing->qty -= quantity;
    standing->totalFilled += quantity;
    incoming->qty -= quantity;
    incoming->totalFilled += quantity;

    price = standing->price;

    fill = init_fill(price, quantity, ts);

    // Figure out where to put the fill...

    if (standing->firstfillnode == NULL)
    {
        standing->firstfillnode = init_fillnode(fill, NULL, NULL);
    } else {
        currentfillnode = standing->firstfillnode;
        while (currentfillnode->next != NULL)
        {
            currentfillnode = currentfillnode->next;
        }
        currentfillnode->next = init_fillnode(fill, currentfillnode, NULL);
    }

    // Again for other order...

    if (incoming->firstfillnode == NULL)
    {
        incoming->firstfillnode = init_fillnode(fill, NULL, NULL);
    } else {
        currentfillnode = incoming->firstfillnode;
        while (currentfillnode->next != NULL)
        {
            currentfillnode = currentfillnode->next;
        }
        currentfillnode->next = init_fillnode(fill, currentfillnode, NULL);
    }

    if (standing->qty == 0) standing->open = 0;
    if (incoming->qty == 0) incoming->open = 0;

    // Fix the positions of the 2 accounts...

    if (strcmp(standing->account->name, incoming->account->name))       // Transactions with self do nothing
    {
        if (standing->direction == BUY)
        {
            update_account(standing->account, quantity, price, BUY);
            update_account(incoming->account, quantity, price, SELL);
        } else {
            update_account(standing->account, quantity, price, SELL);
            update_account(incoming->account, quantity, price, BUY);
        }
    }

    set_quote_lastinfo(price, quantity);    // The rest of the quote will be generated by the function
                                            // execute_order() when the whole execution is finished

    create_execution_messages(standing, incoming, quantity, price, ts);

    return;
}


void run_order (ORDER * order)
{
    LEVEL * current_level;
    ORDERNODE * current_node;

    if (order->direction == SELL)
    {
        for (current_level = FirstBidLevel; current_level != NULL; current_level = current_level->next)
        {
            if (current_level->price < order->price && order->orderType != MARKET) return;

            for (current_node = current_level->firstordernode; current_node != NULL; current_node = current_node->next)
            {
                cross(current_node->order, order);
                if (order->open == 0) return;
            }
        }
    } else {
        for (current_level = FirstAskLevel; current_level != NULL; current_level = current_level->next)
        {
            if (current_level->price > order->price && order->orderType != MARKET) return;

            for (current_node = current_level->firstordernode; current_node != NULL; current_node = current_node->next)
            {
                cross(current_node->order, order);
                if (order->open == 0) return;
            }
        }
    }

    return;
}


void cleanup_closed_bids_or_asks (LEVEL ** root_level)  // root_level is pointing to FirstBidLevel or FirstAskLevel, themselves pointers
{
    LEVEL * current_level;
    LEVEL * old_level;
    ORDERNODE * current_node;
    ORDERNODE * old_node;

    if (*root_level == NULL) return;

    current_level = *root_level;                        // Due to the double indirection, current_level still a pointer to a LEVEL
    current_node = current_level->firstordernode;
    assert(current_node != NULL);

    while (1)
    {
        if (current_node->order->open)                  // We found the very first (highest priority) open order
        {
            current_level->firstordernode = current_node;
            current_level->prev = NULL;
            current_node->prev = NULL;
            *root_level = current_level;                // Remembering that current_level is a pointer, so
            return;                                     // FirstBidLevel or FirstAskLevel ends up pointing to the level
        }

        if (current_node->next != NULL)
        {
            old_node = current_node;
            current_node = current_node->next;
            free(old_node);
        } else {
            old_node = current_node;
            old_level = current_level;
            current_level = current_level->next;
            free(old_node);
            free(old_level);
            if (current_level != NULL)
            {
                current_node = current_level->firstordernode;
                assert(current_node != NULL);
            } else {
                *root_level = NULL;
                return;
            }
        }
    }

    return;
}


void insert_ask (ORDER * order)
{
    ORDERNODE * ordernode;
    ORDERNODE * current_node;
    LEVEL * prev_level = NULL;
    LEVEL * level;
    LEVEL * newlevel;

    ordernode = init_ordernode(order, NULL, NULL);      // Fix ->prev later

    if (FirstAskLevel == NULL)
    {
        FirstAskLevel = init_level(order->price, ordernode, NULL, NULL);
        return;
    } else {
        level = FirstAskLevel;
    }

    while (1)
    {
        if (order->price < level->price)
        {
            // Create new level...

            newlevel = init_level(order->price, ordernode, prev_level, level);
            level->prev = newlevel;
            if (prev_level)
            {
                prev_level->next = newlevel;
            } else {
                FirstAskLevel = newlevel;
            }
            return;

        } else if (order->price == level->price) {

            break;

        } else {

            // Iterate...

            prev_level = level;
            if (level->next != NULL)
            {
                level = level->next;
            } else {
                level->next = init_level(order->price, ordernode, prev_level, NULL);
                return;
            }
        }
    }

    // So we should be on the right level now

    assert(level->price == order->price);

    current_node = level->firstordernode;
    assert(current_node != NULL);

    while (current_node->next != NULL)
    {
        current_node = current_node->next;
    }

    current_node->next = ordernode;
    ordernode->prev = current_node;

    return;
}


void insert_bid (ORDER * order)
{
    ORDERNODE * ordernode;
    ORDERNODE * current_node;
    LEVEL * prev_level = NULL;
    LEVEL * level;
    LEVEL * newlevel;

    ordernode = init_ordernode(order, NULL, NULL);      // Fix ->prev later

    if (FirstBidLevel == NULL)
    {
        FirstBidLevel = init_level(order->price, ordernode, NULL, NULL);
        return;
    } else {
        level = FirstBidLevel;
    }

    while (1)
    {
        if (order->price > level->price)
        {
            // Create new level...

            newlevel = init_level(order->price, ordernode, prev_level, level);
            level->prev = newlevel;
            if (prev_level)
            {
                prev_level->next = newlevel;
            } else {
                FirstBidLevel = newlevel;
            }
            return;

        } else if (order->price == level->price) {

            break;

        } else {

            // Iterate...

            prev_level = level;
            if (level->next != NULL)
            {
                level = level->next;
            } else {
                level->next = init_level(order->price, ordernode, prev_level, NULL);
                return;
            }
        }
    }

    // So we should be on the right level now

    assert(level->price == order->price);

    current_node = level->firstordernode;
    assert(current_node != NULL);

    while (current_node->next != NULL)
    {
        current_node = current_node->next;
    }

    current_node->next = ordernode;
    ordernode->prev = current_node;

    return;
}


int fok_can_buy (int qty, int price)
{
    // Must use subtraction only. Adding could overflow.

    LEVEL * level;
    ORDERNODE * ordernode;

    for (level = FirstAskLevel; level != NULL && level->price <= price; level = level->next)
    {
        for (ordernode = level->firstordernode; ordernode != NULL; ordernode = ordernode->next)
        {
            qty -= ordernode->order->qty;
            if (qty <= 0) return 1;
        }
    }

    return 0;
}


int fok_can_sell (int qty, int price)
{
    // Must use subtraction only. Adding could overflow.

    LEVEL * level;
    ORDERNODE * ordernode;

    for (level = FirstBidLevel; level != NULL && level->price >= price; level = level->next)
    {
        for (ordernode = level->firstordernode; ordernode != NULL; ordernode = ordernode->next)
        {
            qty -= ordernode->order->qty;
            if (qty <= 0) return 1;
        }
    }

    return 0;
}


ACCOUNT * init_account (char * name)
{
    ACCOUNT * ret;

    DebugInfo.inits_of_account++;

    ret = malloc(sizeof(ACCOUNT));
    check_ptr_or_quit(ret);

    safe_strcpy(ret->name, name, SMALLSTRING);

    ret->orders = NULL;
    ret->arraylen = 0;
    ret->count = 0;

    ret->posmin = 0;
    ret->posmax = 0;
    ret->shares = 0;
    ret->cents = 0;

    return ret;
}


ACCOUNT * account_lookup_or_create (char * account_name, int account_int)
{
    int n;

    // If account_id is too high, we will need more storage...

    while (account_int >= CurrentAccountArrayLen)
    {
        AllAccounts = realloc(AllAccounts, (CurrentAccountArrayLen + 64) * sizeof(ACCOUNT *));
        check_ptr_or_quit(AllAccounts);
        CurrentAccountArrayLen += 64;

        // We must NULLify our new account pointers because there can be holes in the known
        // account IDs: e.g. known IDs are 0,1,2,3,7. So, if we're asked to lookup ID 5, we
        // need a way to know it doesn't exist...

        for (n = CurrentAccountArrayLen - 64; n < CurrentAccountArrayLen; n++)
        {
            AllAccounts[n] = NULL;
        }

        DebugInfo.reallocs_of_global_account_list++;
    }

    // If the account corresponsing to the account_id is NULL, create it...

    if (AllAccounts[account_int] == NULL)
    {
        AllAccounts[account_int] = init_account(account_name);
    }

    // Done...

    return AllAccounts[account_int];
}


void add_order_to_account (ORDER * order, ACCOUNT * accountobject)
{
    if (accountobject->count == accountobject->arraylen)
    {
        accountobject->orders = realloc(accountobject->orders, (accountobject->arraylen + 256) * sizeof(ORDER *));
        check_ptr_or_quit(accountobject->orders);
        accountobject->arraylen += 256;

        DebugInfo.reallocs_of_account_order_list++;
    }
    accountobject->orders[accountobject->count] = order;
    accountobject->count += 1;

    return;
}


ORDER_AND_ERROR * execute_order (char * account_name, int account_int, int qty, int price, int direction, int orderType)
{
    // Note: account_name will be in the stack of the calling function, not in the heap

    ORDER * order;
    ORDER_AND_ERROR * o_and_e;
    int id;
    ACCOUNT * accountobject;

    // The o_and_e structure lets us send either an order or an error to the caller...

    o_and_e = init_o_and_e();

    // Check for too high an order ID, too high an account ID, or silly values...

    if (next_id(1) >= MAXORDERS)                // Pass the no-iterate flag to next_id() here
    {                                           // i.e. don't iterate until we know order succeeds
        o_and_e->error = TOO_MANY_ORDERS;
        return o_and_e;

    } else if (account_int >= MAXACCOUNTS)
    {
        o_and_e->error = TOO_HIGH_ACCOUNT;
        return o_and_e;

    } else if (price < 0 || qty < 1 || (direction != SELL && direction != BUY))
    {
        o_and_e->error = SILLY_VALUE;
        return o_and_e;
    }

    // The following call gets the account object. If not already extant, it is created.
    // If more memory is needed to store accounts up to this account_id, that happens...

    accountobject = account_lookup_or_create(account_name, account_int);

    // Create order struct, and store a pointer to it in the account...

    id = next_id(0);
    order = init_order(accountobject, qty, price, direction, orderType, id);
    add_order_to_account(order, accountobject);

    // Run the order, with checks for FOK if needed...

    if (order->orderType != FOK)
    {
        run_order(order);
    } else {
        if (order->direction == BUY)
        {
            if (fok_can_buy(order->qty, order->price))
            {
                run_order(order);
            }
        } else {
            if (fok_can_sell(order->qty, order->price))
            {
                run_order(order);
            }
        }
    }

    // Iterate through the Bids or Asks as appropriate, removing them from the book if they are now closed...

    if (order->direction == SELL)
    {
        cleanup_closed_bids_or_asks(&FirstBidLevel);
    } else {
        cleanup_closed_bids_or_asks(&FirstAskLevel);
    }

    // Market orders get set to price == 0 in official for storage / reporting
    // (the timing doesn't matter, this could be done before running the order)

    if (order->orderType == MARKET) order->price = 0;

    // Place open limit orders on the book. Mark other order types as closed...

    if (order->open)
    {
        if (order->orderType == LIMIT)
        {
            if (order->direction == SELL)
            {
                insert_ask(order);
            } else {
                insert_bid(order);
            }
        } else {
            order->open = 0;
            order->qty = 0;
        }
    }

    // If something happened, fix the quote and fire a ticker WebSocket message.
    // The definition of "something happened" is anything that changes the book:
    //      - a limit order was placed, OR
    //      - fills were generated
    // Nothing else changes the book except cancels, which we aren't dealing with here.

    if (order->totalFilled || order->orderType == LIMIT)
    {
        remake_most_of_quote();     // the "last trade" parts are done by cross()
        create_ticker_message();
    }

    o_and_e->order = order;
    return o_and_e;
}


LEVEL * find_level (int price, int dir)      // Return ptr to level, or return NULL if not present
{
    LEVEL * level = NULL;

    if (dir == BUY)
    {
        level = FirstBidLevel;
        while (level != NULL)
        {
            if (level->price > price)
            {
                level = level->next;
            } else if (level->price == price) {
                break;
            } else {
                level = NULL;
                break;
            }
        }
    } else {
        level = FirstAskLevel;
        while (level != NULL)
        {
            if (level->price < price)
            {
                level = level->next;
            } else if (level->price == price) {
                break;
            } else {
                level = NULL;
                break;
            }
        }
    }

    return level;
}


ORDERNODE * find_ordernode (LEVEL * level, int id)
{
    ORDERNODE * ordernode;

    if (level)
    {
        for (ordernode = level->firstordernode; ordernode != NULL; ordernode = ordernode->next)
        {
            if (ordernode->order->id == id)
            {
                return ordernode;
            }
        }
    }

    return NULL;
}


void cleanup_after_cancel (ORDERNODE * ordernode, LEVEL * level)       // Free the ordernode, maybe free the level, fix all links
{
    int dir;

    assert(ordernode && level);

    dir = ordernode->order->direction;                  // Needed later


    if (ordernode->prev)
    {
        ordernode->prev->next = ordernode->next;
    } else {
        level->firstordernode = ordernode->next;        // Can set level->firstordernode to NULL, in which case
    }                                                   // the level is now empty and must be destroyed in a bit

    if (ordernode->next)
    {
        ordernode->next->prev = ordernode->prev;
    }

    free(ordernode);

    if (level->firstordernode == NULL)
    {
        if (level->prev)
        {
            level->prev->next = level->next;
        } else {
            if (dir == BUY)
            {
                FirstBidLevel = level->next;
            } else {
                FirstAskLevel = level->next;
            }
        }

        if (level->next)
        {
            level->next->prev = level->prev;
        }

        free(level);
    }

    return;
}


void print_orderbook_binary (void)
{
    /*
    Strategy for binary printout of the orderbook. Qty is never 0, so 0 qty can be used as an in-channel flag.

    Format then is:

    all bids ... flag ... all asks ... flag
    using 8 bytes per message (e.g. 1 order or 1 flag takes 8 bytes)

    e.g for a book with 2 bids and 2 asks:

    4bytes (qty)
    4bytes (price)
    4bytes (qty)
    4bytes (price)
    0x00000000 (zero qty: flag)
    0x00000000 (for consistency, i.e. 8 bytes per message)
    4bytes (qty)
    4bytes (price)
    4bytes (qty)
    4bytes (price)
    0x00000000 (zero qty: flag)
    0x00000000 (for consistency, i.e. 8 bytes per message)

    Since we must choose an endian system, we will choose BIG (go big endian or go home).
    */

    LEVEL * level;
    ORDERNODE * ordernode;

    int i;
    int n;
    uint32_t qty;       // the order qty and price are signed ints not exceeding 2^31-1
    uint32_t price;     // but promotion to unsigned here seems perfectly fine

    for (i = 0; i < 2; i++)
    {
        for (level = (i == 0 ? FirstBidLevel : FirstAskLevel); level != NULL; level = level->next)
        {
            for (ordernode = level->firstordernode; ordernode != NULL; ordernode = ordernode->next)
            {
                qty = (uint32_t) ordernode->order->qty;
                putc((qty & 0xFF000000) >> 24, stdout);
                putc((qty & 0x00FF0000) >> 16, stdout);
                putc((qty & 0x0000FF00) >>  8, stdout);
                putc((qty & 0x000000FF)      , stdout);

                price = (uint32_t) ordernode->order->price;
                putc((price & 0xFF000000) >> 24, stdout);
                putc((price & 0x00FF0000) >> 16, stdout);
                putc((price & 0x0000FF00) >>  8, stdout);
                putc((price & 0x000000FF)      , stdout);
            }
        }

        for (n = 0; n < 8; n++)
        {
            putc('\0', stdout);
        }
    }

    return;
}


void print_all_orders_of_account (ACCOUNT * account)
{
    int flag;
    int n;

    assert(account);

    printf("{\"ok\": true, \"venue\": \"%s\", \"orders\": [", Venue);

    flag = 0;
    for (n = 0; n < account->count; n++)
    {
        if (flag) printf(", \n");
        print_order(stdout, account->orders[n]);
        flag = 1;
    }

    printf("]}");

    return;
}


void cancel_order_by_id (int id)
{
    ORDERNODE * ordernode;
    int price;
    int dir;
    LEVEL * level;

    assert(id >= 0 && id <= HighestKnownOrder);

    if (AllOrders[id]->orderType != LIMIT)          // Everything else is auto-cancelled after running
    {
        return;
    }

    price = AllOrders[id]->price;
    dir = AllOrders[id]->direction;

    // Find the level then the ordernode, if possible...

    level = find_level(price, dir);
    ordernode = find_ordernode(level, id);          // This is safe even if level == NULL

    // Now close the order and do the linked-list fiddling...

    if (ordernode)
    {
        ordernode->order->open = 0;
        ordernode->order->qty = 0;

        cleanup_after_cancel(ordernode, level);     // Frees the node and even the level if needed; fixes links

        remake_most_of_quote();                     // Remakes all but the "last trade" info in the quote
        create_ticker_message();
    }

    return;
}


void print_scores (void)
{
    ACCOUNT * account;
    int64_t nav64;
    char * ts;
    int n;

    printf("<html><head><title>%s %s</title></head><body><pre>%s %s\n", Venue, Symbol, Venue, Symbol);

    if (Quote.last == -1)
    {
        printf("No trading activity yet.</pre>");
        return;
    }

    printf("Current price: $%d.%02d\n\n", Quote.last / 100, Quote.last % 100);

    printf("             Account           USD $          Shares         Pos.min         Pos.max           NAV $\n");

    for (n = 0; n < CurrentAccountArrayLen; n++)
    {
        if (AllAccounts[n])
        {
            account = AllAccounts[n];

            // The values account->shares and account->cents are both int32, as is Quote.last, so
            // account->shares * Quote.last + account->cents is guaranteed to fit in an int64.

            nav64 = (int64_t) account->shares * (int64_t) Quote.last + (int64_t) account->cents;

            printf("%20s %15d %15d %15d %15d %15" PRId64 "\n",
                    account->name, account->cents / 100, account->shares, account->posmin, account->posmax, nav64 / 100);
        }
    }

    ts = new_timestamp();
    printf("\n  Start time: %s\nCurrent time: %s", StartTime, ts);
    free(ts);

    printf("</pre></body></html>");

    return;
}


void print_timestamp (void)
{
    char * ts;
    ts = new_timestamp();
    printf("%s", ts);
    free(ts);
    return;
}


void print_memory_info (void)
{
    printf( "DebugInfo.inits_of_level: %d,\n"               // The compiler auto-concatenates these things
            "DebugInfo.inits_of_fill: %d,\n"                // (note the lack of commas)
            "DebugInfo.inits_of_fillnode: %d,\n"
            "DebugInfo.inits_of_order: %d,\n"
            "DebugInfo.inits_of_ordernode: %d,\n"
            "DebugInfo.inits_of_account: %d,\n"
            "DebugInfo.reallocs_of_global_order_list: %d,\n"
            "DebugInfo.reallocs_of_global_account_list: %d,\n"
            "DebugInfo.reallocs_of_account_order_list: %d",
            DebugInfo.inits_of_level,
            DebugInfo.inits_of_fill,
            DebugInfo.inits_of_fillnode,
            DebugInfo.inits_of_order,
            DebugInfo.inits_of_ordernode,
            DebugInfo.inits_of_account,
            DebugInfo.reallocs_of_global_order_list,
            DebugInfo.reallocs_of_global_account_list,
            DebugInfo.reallocs_of_account_order_list
            );
    return;
}


int main (int argc, char ** argv)
{
    char * eofcheck;
    char * tmp;
    char input[MAXSTRING];
    char tokens[MAXTOKENS][SMALLSTRING];
    int id;
    int n;
    ORDER_AND_ERROR * o_and_e;

    if (argc != 3)
    {
        printf("Backend called with %d arguments (2 required). Quitting.\n", argc - 1);
        return 1;
    }

    // On Windows, set stdout to not auto-convert \n into \r\n (messes with our binary orderbook)
    #if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
    #endif

    safe_strcpy(Venue, argv[1], SMALLSTRING);
    safe_strcpy(Symbol, argv[2], SMALLSTRING);

    StartTime = new_timestamp();

    safe_strcpy(Quote.quoteTime, StartTime, SMALLSTRING);

    while (1)
    {
        eofcheck = fgets(input, MAXSTRING, stdin);

        if (eofcheck == NULL)           // i.e. we HAVE reached EOF
        {
            printf("{\"ok\": false, \"error\": \"Unexpected EOF on stdin. Quitting.\"}");
            end_message(stdout);
            return 1;
        }

        tmp = strtok(input, " \t\n\r");
        for (n = 0; n < MAXTOKENS; n++)
        {
            tokens[n][0] = '\0';        // Clear the token in case there isn't one in this slot
            if (tmp != NULL)
            {
                safe_strcpy(tokens[n], tmp, SMALLSTRING);
                tmp = strtok(NULL, " \t\n\r");
            }
        }

        // Now handle whatever the request was.........

        if (strcmp("ORDER", tokens[0]) == 0)
        {
            o_and_e = execute_order(tokens[1], atoi(tokens[2]), atoi(tokens[3]), atoi(tokens[4]), atoi(tokens[5]), atoi(tokens[6]));
            //                      account    account_int      qty              price            direction        orderType

            if (o_and_e->error)
            {
                printf("{\"ok\": false, \"error\": \"Backend error %d (account = %s, account_int = %d, qty = %d, price = %d, direction = %d, orderType = %d)\"}",
                    o_and_e->error, tokens[1], atoi(tokens[2]), atoi(tokens[3]), atoi(tokens[4]), atoi(tokens[5]), atoi(tokens[6]));
            } else {
                print_order(stdout, o_and_e->order);
            }
            free(o_and_e);

            end_message(stdout);
            continue;
        }

        if (strcmp("ORDERBOOK_BINARY", tokens[0]) == 0)
        {
            print_orderbook_binary();
            fflush(stdout);             // no end_message() call for binary
            continue;
        }

        if (strcmp("STATUS", tokens[0]) == 0)
        {
            id = atoi(tokens[1]);

            if (id < 0 || id > HighestKnownOrder || AllOrders[id] == NULL)
            {
                printf("{\"ok\": false, \"error\": \"No such ID\"}");
            } else {
                print_order(stdout, AllOrders[id]);
            }

            end_message(stdout);
            continue;
        }

        if (strcmp("STATUSALL", tokens[0]) == 0)
        {
            // This can return a stupid amount of data. Frontend might want to not honour requests for this.

            id = atoi(tokens[1]);       // id is an account id in this case

            if (id < 0 || id >= CurrentAccountArrayLen || AllAccounts[id] == NULL)      // The order matters here (short-circuit)
            {
                printf("{\"ok\": false, \"error\": \"Account not known on this book\"}");
            } else {
                print_all_orders_of_account(AllAccounts[id]);
            }

            end_message(stdout);
            continue;
        }

        if (strcmp("CANCEL", tokens[0]) == 0)
        {
            id = atoi(tokens[1]);

            if (id < 0 || id > HighestKnownOrder || AllOrders[id] == NULL)
            {
                printf("{\"ok\": false, \"error\": \"No such ID\"}");
            } else {
                cancel_order_by_id(id);
                print_order(stdout, AllOrders[id]);
            }

            end_message(stdout);
            continue;
        }

        if (strcmp("QUOTE", tokens[0]) == 0)
        {
            print_quote(stdout);
            end_message(stdout);
            continue;
        }

        if (strcmp("__ACC_FROM_ID__", tokens[0]) == 0)
        {
            id = atoi(tokens[1]);

            if (id < 0 || id > HighestKnownOrder || AllOrders[id] == NULL)
            {
                printf("ERROR None");
            } else {
                printf("OK %s", AllOrders[id]->account->name);
            }

            end_message(stdout);
            continue;
        }

        if (strcmp("__DEBUG_MEMORY__", tokens[0]) == 0)
        {
            print_memory_info();
            end_message(stdout);
            continue;
        }

        if (strcmp("__TIMESTAMP__", tokens[0]) == 0)
        {
            print_timestamp();
            end_message(stdout);
            continue;
        }

        if (strcmp("__SCORES__", tokens[0]) == 0)
        {
            print_scores();
            end_message(stdout);
            continue;
        }

        printf("{\"ok\": false, \"error\": \"Did not comprehend\"}");
        end_message(stdout);
        continue;
    }

    return 0;
}
