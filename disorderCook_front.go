package main

import (
    "bufio"
    "encoding/json"
    "fmt"
    "io"
    "net/http"
    "os/exec"
    "strconv"
    "strings"
    "sync"
)

type pipes struct {
    stdin io.WriteCloser
    stdout io.ReadCloser
}

type order struct {
    Symbol      string       `json:"symbol"`
    Stock       string       `json:"stock"`
    Venue       string       `json:"venue"`
    Direction   string       `json:"direction"`
    OrderType   string       `json:"orderType"`
    Account     string       `json:"account"`
    Qty         int32        `json:"qty"`
    Price       int32        `json:"price"`
}

type req_channel struct {
    Venue       string
    Symbol      string
    Command     string
}

const HEARTBEAT_OK = `{"ok": true, "error": ""}`
const UNKNOWN_PATH = `{"ok": false, "error": "Unknown path"}`
const UNKNOWN_VENUE = `{"ok": false, "error": "Unknown venue"}`
const UNKNOWN_SYMBOL = `{"ok": false, "error": "Venue is known but symbol is not"}`
const BAD_JSON = `{"ok": false, "error": "Failed to parse incoming JSON"}`
const URL_MISMATCH = `{"ok": false, "error": "Venue or symbol in URL did not match that in POST"}`
const MISSING_FIELD = `{"ok": false, "error": "Missing key or unacceptable value in POST"}`

const BUY = 1
const SELL = 2

const LIMIT = 1
const MARKET = 2
const FOK = 3
const IOC = 4

// --------------------------------------------------------------------------------------------

var Books = make(map[string]map[string]pipes)
var AccountInts = make(map[string]int)

var C_Process_Lock sync.Mutex

// --------------------------------------------------------------------------------------------

func createbook (venue string, symbol string) {
    
    command := exec.Command("./disorderCook.exe", venue, symbol)
    
    o_pipe, _ := command.StdoutPipe()
    i_pipe, _ := command.StdinPipe()
    
    // Should maybe handle errors from the above. Meh.
    
    if Books[venue] == nil {
        v := make(map[string]pipes)
        Books[venue] = v
    }
    
    Books[venue][symbol] = pipes{i_pipe, o_pipe}
    command.Start()
}

func getresponse (command string, venue string, symbol string) string {
    
    response := ""
    
    v := Books[venue]
    if v == nil {
        return UNKNOWN_VENUE
    }
    
    s := Books[venue][symbol]
    tmp := pipes{nil, nil}
    if s == tmp {
        return UNKNOWN_SYMBOL
    }
    
    reader := bufio.NewReader(Books[venue][symbol].stdout)
    
    C_Process_Lock.Lock()
    
    fmt.Fprintf(Books[venue][symbol].stdin, command)
    
    for {
        nextpiece, _, _ := reader.ReadLine()
        if string(nextpiece) != "END" {
            response += string(nextpiece) + "\n"
        } else {
            break
        }
    }
    
    C_Process_Lock.Unlock()
    
    return response
}

func handler(writer http.ResponseWriter, request * http.Request) {
    
    var command string

    pathlist := strings.Split(request.URL.Path[1:], "/")
    
    // General heartbeat...
    
    if len(pathlist) == 3 {
        if pathlist[2] == "heartbeat" {
            fmt.Fprintf(writer, HEARTBEAT_OK)
            return
        }
    }
    
    // Quote...
    
    if len(pathlist) == 7 {
        if pathlist[2] == "venues" && pathlist[4] == "stocks" && pathlist[6] == "quote" {
            venue := pathlist[3]
            symbol := pathlist[5]
            res := getresponse("QUOTE\n", venue, symbol)
            fmt.Fprintf(writer, res)
            return
        }
    }
    
    // Orderbook...
    
    if len(pathlist) == 6 {
        if pathlist[2] == "venues" && pathlist[4] == "stocks" {
            venue := pathlist[3]
            symbol := pathlist[5]
            res := getresponse("ORDERBOOK\n", venue, symbol)
            fmt.Fprintf(writer, res)
            return
        }
    }
    
    // Status and cancel...
    
    if len(pathlist) == 8 {
        if pathlist[2] == "venues" && pathlist[4] == "stocks" && pathlist[6] == "orders" {
            venue := pathlist[3]
            symbol := pathlist[5]
            id, _ := strconv.Atoi(pathlist[7])
            if request.Method == "DELETE" {
                command = fmt.Sprintf("CANCEL %d\n", id)
            } else {
                command = fmt.Sprintf("STATUS %d\n", id)
            }
            res := getresponse(command, venue, symbol)
            fmt.Fprintf(writer, res)
            return
        }
    }
    
    // Order placing...
    
    if len(pathlist) == 7 {
        if pathlist[2] == "venues" && pathlist[4] == "stocks" && pathlist[6] == "orders" && request.Method == "POST" {
            venue := pathlist[3]
            symbol := pathlist[5]
            
            raw_order := order{}
            decoder := json.NewDecoder(request.Body)
            err := decoder.Decode(&raw_order)
            
            if err != nil {
                fmt.Fprintf(writer, BAD_JSON)
                return
            }

            // Accept missing fields that can be determined from URL...
            if raw_order.Venue == "" {
                raw_order.Venue = venue
            }
            if raw_order.Symbol == "" && raw_order.Stock == "" {
                raw_order.Symbol = symbol
            }
            
            // Accept stock as an alias of symbol...
            if raw_order.Stock != "" {
                raw_order.Symbol = raw_order.Stock
            }
            
            if raw_order.Venue != venue || raw_order.Symbol != symbol {
                fmt.Fprintf(writer, URL_MISMATCH)
                return
            }
            
            if raw_order.Venue == "" || raw_order.Symbol == "" || raw_order.Account == "" || raw_order.Qty == 0 || raw_order.Direction == "" || raw_order.OrderType == "" {
                fmt.Fprintf(writer, MISSING_FIELD)
                return
            }
            
            // FIXME add length checks
            
            int_ordertype := 0
            switch raw_order.OrderType {
                case "ioc":
                    int_ordertype = IOC
                case "immediate-or-cancel":
                    int_ordertype = IOC
                case "fok":
                    int_ordertype = FOK
                case "fill-or-kill":
                    int_ordertype = FOK
                case "limit":
                    int_ordertype = LIMIT
                case "market":
                    int_ordertype = MARKET
            }
            
            int_direction := 0
            switch raw_order.Direction {
                case "sell":
                    int_direction = SELL
                case "buy":
                    int_direction = BUY
            }
            
            acc_id := AccountInts[raw_order.Account]
            if acc_id == 0 {
                acc_id = len(AccountInts) + 1
                AccountInts[raw_order.Account] = acc_id
            }
            
            command = fmt.Sprintf("ORDER %s %d %d %d %d %d\n", raw_order.Account, acc_id, raw_order.Qty, raw_order.Price, int_direction, int_ordertype)
            res := getresponse(command, venue, symbol)
            fmt.Fprintf(writer, res)
            return
        }
    }
    
    fmt.Fprintf(writer, UNKNOWN_PATH)
    return
}

func main() {
    
    createbook("TESTEX", "FOOBAR")
    
    http.HandleFunc("/", handler)
    http.ListenAndServe(":8000", nil)
}