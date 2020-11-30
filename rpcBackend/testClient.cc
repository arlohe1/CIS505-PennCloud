#include <iostream>

#include "rpc/client.h"
#include "rpc/rpc_error.h"

using resp_tuple = std::tuple<int, std::string>;

void isCorrect(const char* exp, std::string resp) {
    if (resp.compare(exp) == 0) {
        std::cout << "----- CORRECT" << std::endl;
    } else {
        std::cout << "----- INCORRECT !!!!!" << std::endl;
    }
}

// need to use this version if @exp string might have null in middle of it
void isCorrectStrings(std::string exp, std::string resp) {
    if (resp.compare(exp) == 0) {
        std::cout << "----- CORRECT" << std::endl;
    } else {
        std::cout << "----- INCORRECT !!!!!" << std::endl;
    }
}

int main() {
    //rpc::client c("localhost", rpc::constants::DEFAULT_PORT);
    rpc::client c("localhost", 10000);

    try {
        std::string rowString("lianap");
        std::string randomNameString("idiot@gmail");
        std::string colString("name");
        std::string valString("Lee");
        std::string middleNullvalString("Lee\0Pat", 7); // NOTE: for creating strings with nulls, need to set length explicitly in constructor
        std::string wrongValString("wrongVal");
        std::string newValString("newVal");
        resp_tuple resp;
        
        // test basic put and get
        std::cout << "put(lianap, name, Lee) = ";
        resp = c.call("put", rowString, colString, valString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("OK", std::get<1>(resp));

        std::cout << "get(lianap, name) = ";
        resp = c.call("get", rowString, colString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("Lee", std::get<1>(resp));

        // test basic delete removes value
        std::cout << "del(lianap, name) = ";
        resp = c.call("del", rowString, colString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("OK", std::get<1>(resp));

        std::cout << "get(lianap, name) = ";
        resp = c.call("get", rowString, colString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("No such row, column pair", std::get<1>(resp));

        //test delete of value not in kv store
        std::cout << "del(random, name) = ";
        resp = c.call("del", rowString, colString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("No such row, column pair", std::get<1>(resp));

        // test cput with wrong row
        std::cout << "cput(idiot@gmail, name) = ";
        resp = c.call("del", randomNameString, colString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("No such row, column pair", std::get<1>(resp));

        // test cput with wrong exp value
        std::cout << "put(lianap, name, Lee) = ";
        resp = c.call("put", rowString, colString, valString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("OK", std::get<1>(resp));

        std::cout << "cput(lianap, name, wrongVal, newVal) = ";
        resp = c.call("cput", rowString, colString, wrongValString, newValString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("Incorrect expVal", std::get<1>(resp));

        // test cput with correct exp value
        std::cout << "cput(lianap, name, Lee, newVal) = ";
        resp = c.call("cput", rowString, colString, valString, newValString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("OK", std::get<1>(resp));

        std::cout << "get(lianap, name) = ";
        resp = c.call("get", rowString, colString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("newVal", std::get<1>(resp));

        // test putting and getting nulls
        std::cout << "put(lianap, name, LeePat) = ";
        resp = c.call("put", rowString, colString, middleNullvalString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("OK", std::get<1>(resp));


        std::cout << "get(lianap, name) = ";
        resp = c.call("get", rowString, colString).as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrectStrings(middleNullvalString, std::get<1>(resp));


        


    } catch (rpc::rpc_error &e) {
        // std::cout << std::endl << e.what() << std::endl;
        // std::cout << "in function '" << e.get_function_name() << "': ";

        // using err_t = std::tuple<int, std::string>;
        // auto err = e.get_error().as<err_t>();
        // std::cout << "[error " << std::get<0>(err) << "]: " << std::get<1>(err)
        //           << std::endl;
        return 1;
    }

    return 0;
}
