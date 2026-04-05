#pragma once

#include <exception>
#include <stdexcept>
#include <stdarg.h>
#include <iostream>


#define cryptothrow(msg, error) throw crypto::crypto_exception(msg, error, __FILE__, __LINE__)


namespace crypto {
    class crypto_exception : public std::runtime_error {
    public:
        crypto_exception(std::string msg, int num, const char* file="", int line=0) : std::runtime_error(msg) {
            printf("crypto_exception file: %s line: %d error: %s \n", file, line, msg.c_str());
            errMsg = msg;
            errNum = num;
        }

        virtual ~crypto_exception() throw() {}

        virtual const char* what() const throw() {
           return errMsg.c_str();
        }

        int getErrorNum() const{
            return errNum;
        }

        std::string getErrorMsg() const{
            return errMsg;
        }

    private:
        std::string errMsg;
        int errNum;
    };
}
