#pragma once

#include <string>


#define NoError 0
#define OrderParamError -1
#define OrderTypeError -2
#define InstTypeError  -3
#define DirectionError -4
#define OffsetFlagError -5
#define OrderIdError -6
#define CancelOrQueryTypeError -7
#define OrderAlreadyFinishedError -8
#define OrderNotFoundError -20
#define CapitalNotEnoughError -21
#define PositionNotEnoughError  -22
#define OrderPOCImmediateError -23
#define TooManyOrdersError -24
#define PriceTooDeviatedError -25
#define NoNeedToModifyOrderError -26
#define HitUpperLowerPriceError -27
#define OpenOrdersTooManyError -28
#define ForbiddenError -29
#define ReduceOnlyOrderError -30
#define NotionalTooSmallError -31
#define SMCInstrumentNotExistError -50
#define ADLError -51
#define TwapError -52
#define LiquidationError -53
#define NetworkError -500
#define NetworkUnknownError -5031
#define NetworkServiceUnavailableError -5032
#define NetworkInternalError -5033
#define OverLoadedError -5034
#define TBDisconnectError -600
#define OMSOrderNotFoundError -99998
#define UnknownError -99999


inline std::string CryptoErrorMsg(int errorId) {
    switch (errorId) {
        case NoError:
            return "Correct";
        case OrderParamError:
            return "OrderParamError";
        case OrderTypeError:
            return "OrderTypeError";
        case InstTypeError:
            return "InstTypeError";
        case DirectionError:
            return "DirectionError";
        case OffsetFlagError:
            return "OffsetFlagError";
        case OrderIdError:
            return "OrderIdError";
        case CancelOrQueryTypeError:
            return "CancelOrQueryTypeError";
        case OrderAlreadyFinishedError:
            return "OrderAlreadyFinishedError";
        case OrderNotFoundError:
            return "OrderNotFoundError";
        case CapitalNotEnoughError:
            return "CapitalNotEnoughError";
        case PositionNotEnoughError:
            return "PositionNotEnoughError";
        case OrderPOCImmediateError:
            return "OrderPOCImmediateError";
        case TooManyOrdersError:
            return "TooManyOrdersError";
        case SMCInstrumentNotExistError:
            return "SMCInstrumentNotExistError";
        case NetworkError:
            return "NetworkError";
        case NetworkUnknownError:
            return "NetworkUnknownError";
        case NetworkServiceUnavailableError:
            return "NetworkServiceUnavailableError";
        case NetworkInternalError:
            return "NetworkInternalError";
        case TBDisconnectError:
            return "TBDisconnectError";
        case UnknownError:
            return "UnknownError";
        default:
            return "UnExpectedError, originMsg:";
    }
}
