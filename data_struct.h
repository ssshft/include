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
    C_SWAP,
    BUSD_SWAP,
    USDC_SWAP,
    BTC_SWAP,
    USDT_FUTURES,
    C_FUTURES,
    BTC_FUTURES,
    OPTION
};

static std::unordered_map<InstType, std::string> InstTypeEnum2StrMap {
    {InstType_MIN, "InstType_MIN"},
    {SPOT, "SPOT"},
    {MARGIN, "MARGIN"},
    {USDT_SWAP, "USDT_SWAP"},
    {C_SWAP, "C_SWAP"},
    {BUSD_SWAP, "BUSD_SWAP"},
    {USDC_SWAP, "USDC_SWAP"},
    {BTC_SWAP, "BTC_SWAP"},
    {USDT_FUTURES, "USDT_FUTURES"},
    {C_FUTURES, "C_FUTURES"},
    {BTC_FUTURES, "BTC_FUTURES"},
    {OPTION, "OPTION"}
};

static std::unordered_map<std::string, InstType> InstTypeStr2EnumMap {
    {"InstType_MIN", InstType_MIN},
    {"SPOT", SPOT},
    {"MARGIN", MARGIN},
    {"USDT_SWAP", USDT_SWAP},
    {"C_SWAP", C_SWAP},
    {"BUSD_SWAP", BUSD_SWAP},
    {"USDC_SWAP", USDC_SWAP},
    {"BTC_SWAP", BTC_SWAP},
    {"USDT_FUTURES", USDT_FUTURES},
    {"C_FUTURES", C_FUTURES},
    {"BTC_FUTURES", BTC_FUTURES},
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
        char instId[INSTID_SIZE];
        char originInstId[INSTID_SIZE];
        char base[INSTID_SIZE];
        char quote[INSTID_SIZE];
        char margin[INSTID_SIZE];
        double value;//合约面值
        double tickSize;//价格精度，比如0.001
        double lotSize;//下单数量精度，比如0.00001
        double minSize;//下单最小数量
        double maxSize;//最大下单数量
        double minAmount;//最小下单金额
        double magnifyNumber;//放大倍数
        double reduceNumber;//magnifyNumber倒数

        std::string getString() {
            std::string s = fmt::format("{},{},{},{},{},{},{},"
                            "{},{},{},{},{},{},{},{}",
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
                            reduceNumber
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
            
            writer.EndObject();
            
            return buffer.GetString();
        }
    };

    struct MDBase {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        MarketType marketTypeEnum;
        char instId[INSTID_SIZE];

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

        string getString() {
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
        string getString() {
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

        string getString() {
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
        string getString() {
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

        string getString() {
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

        string getString() {
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

        string getString() {
            string ret{""};
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

        string getString(){
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

namespace om {
    struct OM_BASE {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];
    };

    struct Balance : public OM_BASE{
        char currency[CCY_SIZE];
        //总数
        double total;
        //下单可用余额
        double available;
        //未实现盈亏
        double unrealizedPnl;
        //冻结
        double frozen;
        //来源
        ApiSource apiSourceEnum;

        int64_t updateTime;

        std::string getString(){
            std::string ret;
            // ret.append("[Balance]exchId:").append(getExchIdStr())
            // .append(",instType:").append(getInstTypeStr()).append(",strategyId:").append(strategyId)
            // .append(",currency:").append(currency)
            // .append(",total:").append(std::to_string(total))
            // .append(",available:").append(std::to_string(available))
            // .append(",unrealizedPnl:").append(std::to_string(unrealizedPnl))
            // .append(",frozen:").append(std::to_string(frozen))
            // .append(",apiSourceEnum:").append(ApiSourceEnum2StrMap[apiSourceEnum])
            // .append(",updateTime:").append(std::to_string(updateTime));
            return ret;
        }
    };

    struct Position : public OM_BASE{
        char instId[INSTID_SIZE];
        Direction direction;
        //持仓数量
        double volume;
        //维持保证金
        double maintMargin;
        //开仓均价
        double avgPrice;
        //未实现盈亏
        double unrealizedPnl;
        //清仓价
        double liquidPrice;
        //标记价格
        double markPrice;
        //adl排名
        double adlQuantile;
        //来源
        ApiSource apiSourceEnum;

        UNIXTIMESTAMP updateTime;

        string getString(){
            string ret{""};
            // ret.append("[Position]exchId:").append(getExchIdStr()).append(",instType:")
            // .append(getInstTypeStr()).append(",strategyId:").append(strategyId)
            // .append(",instId:").append(instId)
            // .append(",direction:").append(DirectionEnum2StrMap[direction])
            // .append(",volume:").append(std::to_string(volume))
            // .append(",maintMargin:").append(std::to_string(maintMargin))
            // .append(",avgPrice:").append(std::to_string(avgPrice))
            // .append(",unrealizedPnl:").append(std::to_string(unrealizedPnl))
            // .append(",liquidPrice:").append(std::to_string(liquidPrice))
            // .append(",markPrice:").append(std::to_string(markPrice))
            // .append(",adlQuantile:").append(std::to_string(adlQuantile))
            // .append(",apiSourceEnum:").append(ApiSourceEnum2StrMap[apiSourceEnum])
            // .append(",updateTime:").append(std::to_string(updateTime));
            return ret;
        }
    };

    struct TotalAccount : public OM_BASE{
        //美金层面权益
        double totalEquity;
        double adjEquity;
        //维持保证金
        double mmr;
        //保证金率
        double mgnRatio;
        //来源
        ApiSource apiSourceEnum;

        UNIXTIMESTAMP updateTime;

        std::string getString(){
            std::string ret;
            // ret.append("[TotalAccount]exchId:").append(getExchIdStr())
            // .append(",instType:").append(getInstTypeStr()).append(",strategyId:").append(strategyId)
            // .append(",totalEquity:").append(std::to_string(totalEquity))
            // .append(",adjEquity:").append(std::to_string(adjEquity))
            // .append(",mmr:").append(std::to_string(mmr))
            // .append(",mgnRatio:").append(std::to_string(mgnRatio))
            // .append(",apiSourceEnum:").append(ApiSourceEnum2StrMap[apiSourceEnum])
            // .append(",updateTime:").append(std::to_string(updateTime));
            return ret;
        }
    };

    
    struct OrderTrade : public OM_BASE{
        char instId[INSTID_SIZE];
        //用户自定义id
        CLIENT_ORDER_ID_TYPE clientOrderId;
        //oms产生的唯一id，与clientOrderId配对
        char orderSysId[ORDER_SIZE];
        //交易所订单 id
        char orderId[ORDER_SIZE];
        //策略留存
        char strategyRef[ORDER_SIZE];
        //OPEN CLOSE CANCEL
        OffsetFlag offsetFlag;
        //LONG SHORT
        Direction direction;
        //订单类型：现价单 市价单 等
        OrderType orderType;
        //订单状态
        OrderStatus orderStatus;
        //挂单总数量
        double volumeTotal;
        //挂单价格
        double limitPrice;
        //是否只减仓
        bool reduceOnly;
        //trade相关
        //交易id
        // char tradeId[ORDER_SIZE];
        //交易平均价格
        double tradePrice;
        //累计已成交数量
        double volumeTraded;
        //交易费用
        // double tradeFee;

        //是否是maker
        bool isMaker;
        //交易时间
        // UNIXTIMESTAMP tradeTime;
        //本次交易数量
        double tradedDiff;

        //来源
        ApiSource apiSourceEnum;
        //订单插入时间
        UNIXTIMESTAMP insertTime;
        //订单最后更新时间
        UNIXTIMESTAMP updateTime;
        //rest系统发出时刻
        UNIXTIMESTAMP tsSent;
        //rest数据到达时刻
        UNIXTIMESTAMP tsNet;
        //错误代码
        int ErrorID;
        //原始错误信息
        char originMsg[ORIGINMSG_SIZE];

        std::string getString(){
            char ret[2048];
            // sprintf(ret,"[OrderTrade]exchId:%s,instType:%s,strategyId:%s,accountId:%s,strategyRef:%s,instId:%s,offsetFlag:%s,direction:%s,"
            // "orderType:%s,orderId:%s,orderSysId:%s,clientOrderId:%ld,limitPrice:%.9f,volumeTotal:%.9f,tradePrice:%.9f,volumeTraded:%.9f,tradedDiff:%.9f"
            // ",orderStatus:%s,reduceOnly:%s,apiSourceEnum:%s,ErrorID:%d,ErrMsg:%s,originMsg:%s,insertTime:%ld,updateTime:%ld,"
            // "tsSent:%ld,tsNet:%ld",
            //     getExchIdStr().c_str(),
            //     getInstTypeStr().c_str(),
            //     strategyId,
            //     accountId,
            //     strategyRef,
            //     instId,
            //     OffsetFlagEnum2StrMap[offsetFlag].c_str(),
            //     DirectionEnum2StrMap[direction].c_str(),
            //     OrderTypeEnum2StrMap[orderType].c_str(),
            //     orderId, orderSysId, clientOrderId,
            //     limitPrice,volumeTotal,
            //     tradePrice,volumeTraded,
            //     tradedDiff,
            //     OrderStatusEnum2StrMap[orderStatus].c_str(),
            //     reduceOnly ? "true" : "false",
            //     ApiSourceEnum2StrMap[apiSourceEnum].c_str(),
            //     ErrorID,
            //     CryptoErrorMsg(ErrorID).c_str(),
            //     originMsg,
            //     insertTime,updateTime,
            //     tsSent,tsNet
            // );
            return ret;
        }
    };
};

struct AccountCfg {
    ExchangeType exchangeTypeEnum;
    InstType instTypeEnum;
	std::string accountId{""};
    std::string strategyId{""};
	std::string apiKey{""};
	std::string secretKey{""};
    std::string password{""};
    std::string userId{""};
    bool isSimulated{false};
	std::string restUrl{""};
	std::string wsUrl{""};
};
