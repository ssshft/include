#include "python_interface/include/python_interface.h"

//using namespace std;
//using namespace boost::python;

struct TestTCommand {
    //交易所类型，如BINANCE，GATEIO
    ExchangeType exchangeTypeEnum;
    //交易对类型，如SPOT现货，U_SWAP usdt永续，C_SWAP 币本位永续，U_FUTURES usdt交割，C_FUTURES 币本位交割
    InstType instTypeEnum;
    //command类型
    pubsub::CommandType cmdTypeEnum;
    //账号ID
    char accountId[64];

    long long insertTime;

    string getAccountId(){
        return accountId;
    }

    void setAccountId(std::string val){
        strncpy(accountId, val.c_str(), 64);
    }
};

//typedef pubsub::PubSuber<pubsub::RCommand> RC_QUEUE;
typedef pubsub::PubSuber<pubsub::TCommand> TC_QUEUE;

typedef pubsub::PubSuber<TestTCommand> TestTCommand_QUEUE;
struct TestPubSuber{
    char* topicStr;
    TestTCommand_QUEUE *m_pTqueue;

    TestPubSuber(char* topicStr) : topicStr(topicStr){
        m_pTqueue = new TestTCommand_QUEUE(topicStr);
    }

    ~TestPubSuber(){
        if(m_pTqueue != nullptr){
            delete m_pTqueue;
        }
    }

    bool pop(TestTCommand &cmd) {
        return m_pTqueue->pop(cmd);
    }

    void push(const TestTCommand &cmd) {
        m_pTqueue->push(cmd);
    }
};

BOOST_PYTHON_MODULE(test_pubsub){
    class_<TestTCommand>("TestTCommand",init<>())
    .def_readwrite("exchangeTypeEnum",&TestTCommand::exchangeTypeEnum)
    .def_readwrite("instTypeEnum",&TestTCommand::instTypeEnum)
    .def_readwrite("cmdTypeEnum",&TestTCommand::cmdTypeEnum)
//    .def_readwrite("accountId",&TestTCommand::accountId)
    .def("getAccountId", &TestTCommand::getAccountId)
    .def("setAccountId", &TestTCommand::setAccountId)
    .ADD_PROPERTY(TestTCommand, accountId)
    .def_readwrite("insertTime",&TestTCommand::insertTime)
    ;

    class_<TestPubSuber>("TestPubSuber",init<char*>())
    //.def(init<int,int,bool,bool>())
    .def_readwrite("topicStr",&TestPubSuber::topicStr)
    .def("pop", &TestPubSuber::pop)
    .def("push", &TestPubSuber::push);
}




