#pragma once

#include "crypto_errors.h"
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/document.h>
#include <fmt/format.h> 
#include <unordered_map>


#ifdef NEED_MBP
    #define MD_LENGTH 131072
#else
    #define MD_LENGTH 32768
#endif

#define INSTID_SIZE 32
#define ACCOUNTID_SIZE 32
#define STRATEGYID_SIZE 32
#define ORDER_SIZE 64
#define MULTI_ORDER_SIZE 512
#define CCY_SIZE 16

#define ORIGINMSG_SIZE 256
#define UNIXTIMESTAMP int64_t
#define CLIENT_ORDER_ID_TYPE int64_t


using namespace std;

enum ExchangeType{
    ExchangeType_MIN=0,
    BINANCE,
    OKX,
    GATEIO,
    BYBIT,
    HTX,
    BITGET
};

static std::unordered_map<ExchangeType, std::string> ExchangeTypeEnum2StrMap {
    {ExchangeType_MIN, "ExchangeType_MIN"},
    {BINANCE, "BINANCE"},
    {OKX, "OKX"},
    {GATEIO, "GATEIO"},
    {BYBIT, "BYBIT"},
    {HTX, "HTX"},
    {BITGET, "BITGET"}
};

static std::unordered_map<std::string, ExchangeType> ExchangeTypeStr2EnumMap {
    {"ExchangeType_MIN", ExchangeType_MIN},
    {"BINANCE", BINANCE},
    {"OKX", OKX},
    {"GATEIO", GATEIO},
    {"BYBIT", BYBIT},
    {"HTX", HTX},
    {"BITGET", BITGET}
};

enum InstType {
    InstType_MIN=0,
    SPOT,
    MARGIN,
    USDT_SWAP,
    USDC_SWAP,
    BUSD_SWAP,
    C_SWAP,
    USDT_FUTURES,
    BUSD_FUTURES
    C_FUTURES,
    OPTION
};

static std::unordered_map<InstType, std::string> InstTypeEnum2StrMap {
    {InstType_MIN, "InstType_MIN"},
    {SPOT, "SPOT"},
    {MARGIN, "MARGIN"},
    {USDT_SWAP, "USDT_SWAP"},
    {USDC_SWAP, "USDC_SWAP"},
    {BUSD_SWAP, "BUSD_SWAP"},
    {C_SWAP, "C_SWAP"},
    {USDT_FUTURES, "USDT_FUTURES"},
    {BUSD_FUTURES, "BUSD_FUTURES"},
    {C_FUTURES, "C_FUTURES"},
    {OPTION, "OPTION"}
};

static std::unordered_map<std::string, InstType> InstTypeStr2EnumMap {
    {"InstType_MIN", InstType_MIN},
    {"SPOT", SPOT},
    {"MARGIN", MARGIN},
    {"USDT_SWAP", USDT_SWAP},
    {"USDC_SWAP", USDC_SWAP},
    {"BUSD_SWAP", BUSD_SWAP},
    {"C_SWAP", C_SWAP},
    {"USDT_FUTURES", USDT_FUTURES},
    {"BUSD_FUTURES", BUSD_FUTURES},
    {"C_FUTURES", C_FUTURES},
    {"OPTION", OPTION}
};

enum OrderType{
    OT_MIN=0,
    OT_LIMIT,
    OT_MARKET,
    OT_POST_ONLY,
    OT_FOK,
    OT_IOC
};

static std::unordered_map<OrderType, std::string> OrderTypeEnum2StrMap {
    {OT_MIN, "OT_MIN"},
    {OT_LIMIT, "OT_LIMIT"},
    {OT_MARKET, "OT_MARKET"},
    {OT_POST_ONLY, "OT_POST_ONLY"},
    {OT_FOK, "OT_FOK"},
    {OT_IOC, "OT_IOC"}
};

static std::unordered_map<std::string, OrderType> OrderTypeStr2EnumMap {
    {"OT_MIN", OT_MIN},
    {"OT_LIMIT", OT_LIMIT},
    {"OT_MARKET", OT_MARKET},
    {"OT_POST_ONLY", OT_POST_ONLY},
    {"OT_FOK", OT_FOK},
    {"OT_IOC", OT_IOC}
};

enum OffsetFlag{
    OF_MIN=0,
    OF_OPEN,
    OF_CLOSE
};

static std::unordered_map<OffsetFlag, std::string> OffsetFlagEnum2StrMap {
    {OF_MIN, "OF_MIN"},
    {OF_OPEN, "OF_OPEN"},
    {OF_CLOSE, "OF_CLOSE"}
};

static std::unordered_map<std::string, OffsetFlag> OffsetFlagStr2EnumMap {
    {"OF_MIN", OF_MIN},
    {"OF_OPEN", OF_OPEN},
    {"OF_CLOSE", OF_CLOSE}
};

enum Direction {
    DT_MIN=0,
    DT_LONG,
    DT_SHORT
};

static std::unordered_map<Direction, std::string> DirectionEnum2StrMap {
    {DT_MIN, "DT_MIN"},
    {DT_LONG, "DT_LONG"},
    {DT_SHORT, "DT_SHORT"}
};

static std::unordered_map<std::string, Direction> DirectionStr2EnumMap {
    {"DT_MIN", DT_MIN},
    {"DT_LONG", DT_LONG},
    {"DT_SHORT", DT_SHORT}
};

enum OrderStatus{
    OS_MIN=0,
    OS_PEND,
    OS_PENDING_NEW,
    OS_NEW,
    OS_PARTFILLED,
    OS_FILLED,
    OS_REJECTED,
    OS_CANCEL,
    OS_CANCELLING,
    OS_CANCELED,
    OS_UNKNOWN,
    OS_FAILED,
    OrderStatus_MAX
};

static std::unordered_map<OrderStatus, std::string> OrderStatusEnum2StrMap {
    {OS_MIN, "OS_MIN"},
    {OS_PEND, "OS_PEND"},
    {OS_PENDING_NEW, "OS_PENDING_NEW"},
    {OS_NEW, "OS_NEW"},
    {OS_PARTFILLED, "OS_PARTFILLED"},
    {OS_FILLED, "OS_FILLED"},
    {OS_REJECTED, "OS_REJECTED"},
    {OS_CANCEL, "OS_CANCEL"},
    {OS_CANCELLING, "OS_CANCELLING"},
    {OS_CANCELED, "OS_CANCELED"},
    {OS_UNKNOWN, "OS_UNKNOWN"},
    {OS_FAILED, "OS_FAILED"}
};

static std::unordered_map<std::string, OrderStatus> OrderStatusStr2EnumMap {
    {"OS_MIN", OS_MIN},
    {"OS_PEND", OS_PEND},
    {"OS_PENDING_NEW", OS_PENDING_NEW},
    {"OS_NEW", OS_NEW},
    {"OS_PARTFILLED", OS_PARTFILLED},
    {"OS_FILLED", OS_FILLED},
    {"OS_REJECTED", OS_REJECTED},
    {"OS_CANCEL", OS_CANCEL},
    {"OS_CANCELLING", OS_CANCELLING},
    {"OS_CANCELED", OS_CANCELED},
    {"OS_UNKNOWN", OS_UNKNOWN},
    {"OS_FAILED", OS_FAILED}
};

enum ApiSource{
    AS_MIN=0,
    AS_ADD_NEW_ORDER,
    AS_CANCEL_ORDER,
    AS_QUERY_ORDER,
    AS_REST,
    AS_WEBSOCKET
};

static std::unordered_map<ApiSource, std::string> ApiSourceEnum2StrMap{
    {AS_MIN, "AS_MIN"},
    {AS_ADD_NEW_ORDER, "AS_ADD_NEW_ORDER"},
    {AS_CANCEL_ORDER, "AS_CANCEL_ORDER"},
    {AS_QUERY_ORDER, "AS_QUERY_ORDER"},
    {AS_REST, "AS_REST"},
    {AS_WEBSOCKET, "AS_WEBSOCKET"}
};

static std::unordered_map<std::string, ApiSource> ApiSourceStr2EnumMap{
    {"AS_MIN", AS_MIN},
    {"AS_ADD_NEW_ORDER", AS_ADD_NEW_ORDER},
    {"AS_CANCEL_ORDER", AS_CANCEL_ORDER},
    {"AS_QUERY_ORDER", AS_QUERY_ORDER},
    {"AS_REST", AS_REST},
    {"AS_WEBSOCKET", AS_WEBSOCKET}
};

namespace md {
    enum MarketType {
        MarketType_MIN,
        TRADES,
        FUNDING_RATE,
        DEPTH1,
        DEPTH5,
        DEPTH10,
        DEPTH20,
        KLINE_1m,
        KLINE_1h,
        KLINE_2h,
        KLINE_4h,
        KLINE_8h,
        MarketType_MAX
    };

    static std::unordered_map<MarketType, std::string> MarketTypeEnum2StrMap {
        {MarketType_MIN, "MarketType_MIN"},
        {TRADES, "TRADES"},
        {FUNDING_RATE, "FUNDING_RATE"},
        {DEPTH1, "DEPTH1"},
        {DEPTH5, "DEPTH5"},
        {DEPTH10, "DEPTH10"},
        {DEPTH20, "DEPTH20"},
        {KLINE_1m, "KLINE_1m"},
        {KLINE_1h, "KLINE_1h"},
        {KLINE_2h, "KLINE_2h"},
        {KLINE_4h, "KLINE_4h"},
        {KLINE_8h, "KLINE_8h"},
    };

    static std::unordered_map<std::string,MarketType> MarketTypeStr2EnumMap {
        {"MarketType_MIN", MarketType_MIN},
        {"TRADES", TRADES},
        {"FUNDING_RATE", FUNDING_RATE},
        {"DEPTH1", DEPTH1},
        {"DEPTH5", DEPTH5},
        {"DEPTH10", DEPTH10},
        {"DEPTH20", DEPTH20},
        {"KLINE_1m", KLINE_1m},
        {"KLINE_1h", KLINE_1h},
        {"KLINE_2h", KLINE_2h},
        {"KLINE_4h", KLINE_4h},
        {"KLINE_8h", KLINE_8h}
    };

    struct InstrumentInfo {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char instId[32];
        char originInstId[32];
        char base[16];
        char quote[16];
        char margin[16];
        double value;//合约面值
        double tickSize;//价格精度，比如0.001
        double lotSize;//下单数量精度，比如0.00001
        double minSize;//下单最小数量
        double maxSize;//最大下单数量
        double minAmount;//最小下单金额
        double magnifyNumber;//放大倍数
        double reduceNumber;//magnifyNumber倒数
        int calcType;       // 现货，usdt本位是0， 币本位是1
        int64_t instIdCode; // okx code, sbe行情用到

        std::string getString() {
            std::string s = fmt::format("{},{},{},{},{},{},{},"
                            "{},{},{},{},{},{},{},{},{},{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum],
                            InstTypeEnum2StrMap[instTypeEnum], 
                            instId, 
                            originInstId, 
                            base, 
                            quote, 
                            margin,
                            value,
                            tickSize,
                            lotSize,
                            minSize,
                            maxSize,
                            minAmount,
                            magnifyNumber,
                            reduceNumber,
                            calcType,
                            instIdCode
                        );
            return s;
        }

        std::string getJsonStr() {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            
            writer.StartObject();
            
            writer.Key("exchangeTypeEnum");
            writer.Int(static_cast<int>(exchangeTypeEnum));
            
            writer.Key("instTypeEnum");
            writer.Int(static_cast<int>(instTypeEnum));
            
            writer.Key("instId");
            writer.String(instId);
            
            writer.Key("originInstId");
            writer.String(originInstId);
            
            writer.Key("base");
            writer.String(base);
            
            writer.Key("quote");
            writer.String(quote);
            
            writer.Key("margin");
            writer.String(margin);
            
            writer.Key("value");
            writer.Double(value);
            
            writer.Key("tickSize");
            writer.Double(tickSize);
            
            writer.Key("lotSize");
            writer.Double(lotSize);
            
            writer.Key("minSize");
            writer.Double(minSize);
            
            writer.Key("maxSize");
            writer.Double(maxSize);
            
            writer.Key("minAmount");
            writer.Double(minAmount);
            
            writer.Key("magnifyNumber");
            writer.Double(magnifyNumber);
            
            writer.Key("reduceNumber");
            writer.Double(reduceNumber);

            writer.Key("calcType");
            writer.Double(calcType);

            writer.Key("instIdCode");
            writer.Double(instIdCode);
            
            writer.EndObject();
            
            return buffer.GetString();
        }
    };

    struct MDBase {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        MarketType marketTypeEnum;
        char instId[32];

        int64_t tsTrans;
        int64_t tsEvent;
        int64_t tsRecv;
        int64_t tsParse;
    };

    struct Depth1 : public MDBase {
        double bp1;
        double ap1;
        double bv1;
        double av1;

        std::string getString() {
            std::string s = fmt::format("exchId:{},instType:{},marketType:{},instId:{},tsTrans:{},tsEvent:{},tsRecv:{},tsParse:{},"
                            "bp1:{},ap1:{},bv1:{},av1:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum],
                            InstTypeEnum2StrMap[instTypeEnum],
                            md::MarketTypeEnum2StrMap[marketTypeEnum],
                            instId,
                            tsTrans, tsEvent, tsRecv, tsParse,
                            bp1, ap1, bv1, av1             
                            );
            return s;
        }
    };

    struct Depth5 : public Depth1 {
        double bp2;
        double bp3;
        double bp4;
        double bp5;

        double ap2;
        double ap3;
        double ap4;
        double ap5;

        double bv2;
        double bv3;
        double bv4;
        double bv5;

        double av2;
        double av3;
        double av4;
        double av5;

        std::string getString() {
            std::string s = fmt::format("exchId:{},instType:{},marketType:{},instId:{},tsTrans:{},tsEvent:{},tsRecv:{},tsParse:{},"
                            "bp1:{},ap1:{},bv1:{},av1:{},"
                            "bp2:{},ap2:{},bv2:{},av2:{},"
                            "bp3:{},ap3:{},bv3:{},av3:{},"
                            "bp4:{},ap4:{},bv4:{},av4:{},"
                            "bp5:{},ap5:{},bv5:{},av5:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum],
                            InstTypeEnum2StrMap[instTypeEnum],
                            md::MarketTypeEnum2StrMap[marketTypeEnum],
                            instId,
                            tsTrans, tsEvent, tsRecv, tsParse,
                            bp1, ap1, bv1, av1,
                            bp2, ap2, bv2, av2,
                            bp3, ap3, bv3, av3,
                            bp4, ap4, bv4, av4,
                            bp5, ap5, bv5, av5             
                            );
            return s;
        }

    };

    struct Depth10 : public Depth5 {
        double bp6;
        double bp7;
        double bp8;
        double bp9;
        double bp10;

        double ap6;
        double ap7;
        double ap8;
        double ap9;
        double ap10;

        double bv6;
        double bv7;
        double bv8;
        double bv9;
        double bv10;

        double av6;
        double av7;
        double av8;
        double av9;
        double av10;

        std::string getString() {
            std::string s = fmt::format("exchId:{},instType:{},marketType:{},instId:{},tsTrans:{},tsEvent:{},tsRecv:{},tsParse:{},"
                            "bp1:{},ap1:{},bv1:{},av1:{},"
                            "bp2:{},ap2:{},bv2:{},av2:{},"
                            "bp3:{},ap3:{},bv3:{},av3:{},"
                            "bp4:{},ap4:{},bv4:{},av4:{},"
                            "bp5:{},ap5:{},bv5:{},av5:{},"
                            "bp6:{},ap6:{},bv6:{},av6:{},"
                            "bp7:{},ap7:{},bv7:{},av7:{},"
                            "bp8:{},ap8:{},bv8:{},av8:{},"
                            "bp9:{},ap9:{},bv9:{},av9:{},"
                            "bp10:{},ap10:{},bv10:{},av10:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum],
                            InstTypeEnum2StrMap[instTypeEnum],
                            md::MarketTypeEnum2StrMap[marketTypeEnum],
                            instId,
                            tsTrans, tsEvent, tsRecv, tsParse,
                            bp1, ap1, bv1, av1,
                            bp2, ap2, bv2, av2,
                            bp3, ap3, bv3, av3,
                            bp4, ap4, bv4, av4,
                            bp5, ap5, bv5, av5, 
                            bp6, ap6, bv6, av6,
                            bp7, ap7, bv7, av7,
                            bp8, ap8, bv8, av8,
                            bp9, ap9, bv9, av9,
                            bp10, ap10, bv10, av10           
                            );
            return s;
        }
    };

    struct Depth20 : public Depth10 {
        double bp11;
        double bp12;
        double bp13;
        double bp14;
        double bp15;
        double bp16;
        double bp17;
        double bp18;
        double bp19;
        double bp20;

        double ap11;
        double ap12;
        double ap13;
        double ap14;
        double ap15;
        double ap16;
        double ap17;
        double ap18;
        double ap19;
        double ap20;

        double bv11;
        double bv12;
        double bv13;
        double bv14;
        double bv15;
        double bv16;
        double bv17;
        double bv18;
        double bv19;
        double bv20;

        double av11;
        double av12;
        double av13;
        double av14;
        double av15;
        double av16;
        double av17;
        double av18;
        double av19;
        double av20;

        std::string getString() {
            std::string s = fmt::format("exchId:{},instType:{},marketType:{},instId:{},tsTrans:{},tsEvent:{},tsRecv:{},tsParse:{},"
                            "bp1:{},ap1:{},bv1:{},av1:{},"
                            "bp2:{},ap2:{},bv2:{},av2:{},"
                            "bp3:{},ap3:{},bv3:{},av3:{},"
                            "bp4:{},ap4:{},bv4:{},av4:{},"
                            "bp5:{},ap5:{},bv5:{},av5:{},"
                            "bp6:{},ap6:{},bv6:{},av6:{},"
                            "bp7:{},ap7:{},bv7:{},av7:{},"
                            "bp8:{},ap8:{},bv8:{},av8:{},"
                            "bp9:{},ap9:{},bv9:{},av9:{},"
                            "bp10:{},ap10:{},bv10:{},av10:{},"
                            "bp11:{},ap11:{},bv11:{},av11:{},"
                            "bp12:{},ap12:{},bv12:{},av12:{},"
                            "bp13:{},ap13:{},bv13:{},av13:{},"
                            "bp14:{},ap14:{},bv14:{},av14:{},"
                            "bp15:{},ap15:{},bv15:{},av15:{},"
                            "bp16:{},ap16:{},bv16:{},av16:{},"
                            "bp17:{},ap17:{},bv17:{},av17:{},"
                            "bp18:{},ap18:{},bv18:{},av18:{},"
                            "bp19:{},ap19:{},bv19:{},av19:{},"
                            "bp20:{},ap20:{},bv20:{},av20:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum],
                            InstTypeEnum2StrMap[instTypeEnum],
                            md::MarketTypeEnum2StrMap[marketTypeEnum],
                            instId,
                            tsTrans, tsEvent, tsRecv, tsParse,
                            bp1, ap1, bv1, av1,
                            bp2, ap2, bv2, av2,
                            bp3, ap3, bv3, av3,
                            bp4, ap4, bv4, av4,
                            bp5, ap5, bv5, av5, 
                            bp6, ap6, bv6, av6,
                            bp7, ap7, bv7, av7,
                            bp8, ap8, bv8, av8,
                            bp9, ap9, bv9, av9,
                            bp10, ap10, bv10, av10,
                            bp11, ap11, bv11, av11,
                            bp12, ap12, bv12, av12,
                            bp13, ap13, bv13, av13,
                            bp14, ap14, bv14, av14,
                            bp15, ap15, bv15, av15, 
                            bp16, ap16, bv16, av16,
                            bp17, ap17, bv17, av17,
                            bp18, ap18, bv18, av18,
                            bp19, ap19, bv19, av19,
                            bp20, ap20, bv20, av20 
                            );
            return s;
        }

    };

    struct Trades : public MDBase {
        char tradeId[32];
        double px;
        double sz;
        Direction direction;

        std::string getString() {
            std::string s = fmt::format("exchId:{},instType:{},marketType:{},instId:{},tsTrans:{},tsEvent:{},tsRecv:{},tsParse:{},"
                            "tradeId:{},px:{},sz:{},direction:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum],
                            InstTypeEnum2StrMap[instTypeEnum],
                            md::MarketTypeEnum2StrMap[marketTypeEnum],
                            instId,
                            tsTrans, tsEvent, tsRecv, tsParse,
                            tradeId, px, sz, DirectionEnum2StrMap[direction]           
                            );
            return s;
        }
    };

    struct FundingRate : public MDBase {
        double fundingRate;
        double nextFundingRate;
        int64_t fundingTime;

        std::string getString() {
            std::string s = fmt::format("exchId:{},instType:{},marketType:{},instId:{},tsTrans:{},tsEvent:{},tsRecv:{},tsParse:{},"
                            "fundingRate:{},nextFundingRate:{},fundingTime:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum],
                            InstTypeEnum2StrMap[instTypeEnum],
                            md::MarketTypeEnum2StrMap[marketTypeEnum],
                            instId,
                            tsTrans, tsEvent, tsRecv, tsParse,
                            fundingRate, nextFundingRate, fundingTime             
                            );
            return s;
        }

    };

    struct Kline : public MDBase {
        int64_t barTime;
        double highPrice;
        double lowPrice;
        double openPrice;
        double closePrice;
        double avgPrice;
        double totalVolume;
        double totalAmount;
        bool isFinished;

        std::string getString() {
            std::string s = fmt::format("exchId:{},instType:{},marketType:{},instId:{},tsTrans:{},tsEvent:{},tsRecv:{},tsParse:{},"
                            "barTime:{},highPrice:{},lowPrice:{},openPrice:{},closePrice:{}"
                            "avgPrice:{},totalVolume:{},totalAmount:{},isFinished:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum],
                            InstTypeEnum2StrMap[instTypeEnum],
                            md::MarketTypeEnum2StrMap[marketTypeEnum],
                            instId,
                            tsTrans, tsEvent, tsRecv, tsParse,
                            barTime, highPrice, lowPrice, openPrice, closePrice,
                            avgPrice, totalVolume, totalAmount, isFinished        
                            );
            return s;
        }
    };

    struct MarketDataHeader {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        MarketType marketTypeEnum;
        char instId[INSTID_SIZE];

        std::string getString() {
            std::string ret{""};
            ret.append("Header:exchId")
                .append(ExchangeTypeEnum2StrMap[exchangeTypeEnum]).append(",instType:")
                .append(InstTypeEnum2StrMap[instTypeEnum]).append(",marketType:")
                .append(MarketTypeEnum2StrMap[marketTypeEnum]).append(",instId:")
                .append(instId).append("\n");
            return ret;
        }
    };

    struct CryptoMarketData{
        MarketDataHeader header;
        union MarketDataBody {
            Depth1  depth1;
            Depth5  depth5;
            Depth10 depth10;
            Depth20 depth20;
            FundingRate fundingRate;
            Trades trades;
            Kline kline;
        };
        MarketDataBody body;

        std::string getString(){
            string ret{""};
            if(header.marketTypeEnum == DEPTH1){
                ret.append(body.depth1.getString());
            }
            else if(header.marketTypeEnum == DEPTH5){
                ret.append(body.depth5.getString());
            }
            else if(header.marketTypeEnum == DEPTH10){
                ret.append(body.depth10.getString());
            }
            else if(header.marketTypeEnum == DEPTH20){
                ret.append(body.depth20.getString());
            }
            else if(header.marketTypeEnum == FUNDING_RATE){
                ret.append(body.fundingRate.getString());
            }
            else if(header.marketTypeEnum == TRADES){
                ret.append(body.trades.getString());
            }
            else if(header.marketTypeEnum == KLINE_1m){
                ret.append(body.kline.getString());
            }
            else {

            }
            return ret;
        }
    };
}

enum ApiMode {
    AM_MIN = 0,
    AM_REST = 1,
    AM_WS = 2
};

static std::unordered_map<std::string, ApiMode> ApiModeStr2EnumMap {
    {"AM_MIN", AM_MIN},
    {"REST", AM_REST},
    {"WS", AM_WS}
};

static std::unordered_map<ApiMode, std::string> ApiModeEnum2StrMap {
    {AM_MIN, "AM_MIN"},
    {AM_REST, "REST"},
    {AM_WS, "WS"}
};

struct AccountCfg {
    ExchangeType exchangeTypeEnum;
    InstType instTypeEnum;
	int accountId{0};
    std::string accountName{""};
    std::string strategyId{""};
	std::string apiKey{""};
	std::string secretKey{""};
    std::string password{""};
    std::string userId{""};
    bool isSimulated{false};
	std::string restUrl{""};
	std::string wsUrl{""};
    std::string wsTradeUrl{""};
    ApiMode apiMode{AM_REST};
};
