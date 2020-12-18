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
    rpc::client c1("localhost", 10000);
    rpc::client c2("localhost", 10002);
    rpc::client c3("localhost", 10004);

    try {
        std::string rowString("lianap");
        std::string randomNameString("idiot@gmail");
        std::string colString("name");
        std::string valString("Lee");
        std::string middleNullvalString("Lee\0Pat", 7); // NOTE: for creating strings with nulls, need to set length explicitly in constructor
        std::string wrongValString("wrongVal");
        std::string newValString("newVal");
        resp_tuple resp;
        

        // consistnecy tests
        std::cout << "ON SERV(p=10001): put(lianap, name, Lee) = ";
        resp = c2.call("put", "lianap", "name", "Lee").as<resp_tuple>();
        std::cout << std::get<1>(resp) << std::endl;
        isCorrect("OK", std::get<1>(resp));

        // for (int i = 0; i < 20; i++) {
        //     std::cout << "ON SERV(p=10000) put(lianap, email, Hi! What's Up???) = ";
        //     resp = c1.call("put", "lianap", "email", "Hi! What's Up???").as<resp_tuple>();
        //     std::cout << std::get<1>(resp) << std::endl;
        //     isCorrect("OK", std::get<1>(resp));
        // }

        

        // std::cout << "ON SERV(p=10002) put(lianap, file, homework 2) = ";
        // resp = c.call("put", "lianap", "file", "homework 2").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // // // multiple puts (for log and checkpoint testing - will need to check log and checkpoint files for correct output)
        // std::cout << "put(lianap, name, Lee) = ";
        // resp = c.call("put", "lianap", "name", "Lee").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "put(lianap, email, Hi! What's Up???) = ";
        // resp = c.call("put", "lianap", "email", "Hi! What's Up???").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "put(lianap, file, homework 2) = ";
        // resp = c.call("put", "lianap", "file", "homework 2").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "put(amitl, name, Amit) = ";
        // resp = c.call("put", "amitl", "name", "Amit").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "put(amitl, email, Hi! What's Up???) = ";
        // resp = c.call("put", "amitl", "email", "Hi! What's Up???").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "put(amitl, file, homework 2) = ";
        // resp = c.call("put", "amitl", "file", "homework 2?").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // //test eviction/checkpointing for get
        // std::cout << "ON SERV(p=10000) get(lianap, name) = ";
        // resp = c.call("get", rowString, colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("Lee", std::get<1>(resp));

        // std::cout << "ON SERV(p=10001) get(lianap, email) = ";
        // resp = c2.call("get", rowString, "email").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("Hi! What's Up???", std::get<1>(resp));

        // std::cout << "get(lianap, file) = ";
        // resp = c.call("get", rowString, "file").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("homework 2", std::get<1>(resp));

        // std::cout << "get(amitl, name) = ";
        // resp = c.call("get", "amitl", colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("Amit", std::get<1>(resp));

        // std::cout << "get(amitl, email) = ";
        // resp = c.call("get", "amitl", "email").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("Hi! What's Up???", std::get<1>(resp));

        // std::cout << "get(amitl, file) = ";
        // resp = c.call("get", "amitl", "file").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("homework 2?", std::get<1>(resp));

        // // // // testing log
        // // // for (int i = 0; i < 1; i++) {
        // // //     std::cout << "get(amitl, file) = ";
        // // //     resp = c.call("get", "amitl", "file").as<resp_tuple>();
        // // //     std::cout << std::get<1>(resp) << std::endl;
        // // //     isCorrect("homework 2?", std::get<1>(resp));

        // // //     std::cout << "get(amitl, email) = ";
        // // //     resp = c.call("get", "amitl", "email").as<resp_tuple>();
        // // //     std::cout << std::get<1>(resp) << std::endl;
        // // //     isCorrect("Hi! What's Up???", std::get<1>(resp));
        // // // }

        // // test cput logging/checkpointing/eviction
        // std::cout << "cput(lianap, name, Lee, updatedName) = ";
        // resp = c.call("cput", "lianap", "name", "Lee", "updatedName").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "ON SERV(p=10002) cput(lianap, email, updated email !!) = ";
        // resp = c3.call("cput", "lianap", "email", "Hi! What's Up???", "updated email !!").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "cput(lianap, file, updated file) = ";
        // resp = c.call("cput", "lianap", "file", "homework 2", "updated file").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "cput(amitl, name, Updated Amit) = ";
        // resp = c.call("cput", "amitl", "name", "Amit", "Updated Amit").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "cput(amitl, email, updated Hi! What's Up???) = ";
        // resp = c.call("cput", "amitl", "email", "Hi! What's Up???", "updated Hi! What's Up???").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "cput(amitl, file, homework 2) = ";
        // resp = c.call("cput", "amitl", "file", "homework 2?", "updated homework 2?").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));


        // std::cout << "cput(amitl, email, updated 2x Hi! What's Up???) = ";
        // resp = c.call("cput", "amitl", "email", "updated Hi! What's Up???", "updated 2x Hi! What's Up???").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "ON SERV(p=10002) cput(amitl, file, wrong val, new val try) = ";
        // resp = c3.call("cput", "amitl", "file", "wrong val", "new val try").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("Incorrect expVal", std::get<1>(resp));

        // std::cout << "cput(amitl, files, wrong val, new val try) = ";
        // resp = c.call("cput", "amitl", "files", "wrong val", "new val try").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));

        // std::cout << "get(amitl, email) = ";
        // resp = c.call("get", "amitl", "email").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("updated 2x Hi! What's Up???", std::get<1>(resp));


        // // // delete tests
        // std::cout << "ON SERV(p=10002) del(amitl, email) = ";
        // resp = c3.call("del", "amitl", "email").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // // delete something not in local kvMap
        // std::cout << "ON SERV(p=10001) del(amitl, file) = ";
        // resp = c2.call("del", "amitl", "file").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "del(amitl, name) = ";
        // resp = c.call("del", "amitl", "name").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // // try to delete again
        // std::cout << "del(amitl, name) = ";
        // resp = c.call("del", "amitl", "name").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));

        // // try to delete random thingy
        // std::cout << "del(amitl, nonexistentCol) = ";
        // resp = c.call("del", "amitl", "nonexistentCol").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));

        // std::cout << "get(amit, email) = ";
        // resp = c.call("get", "amitl", "email").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));

        // std::cout << "get(amit, name) = ";
        // resp = c.call("get", "amitl", "name").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));

        // std::cout << "get(amit, file) = ";
        // resp = c.call("get", "amitl", "file").as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));




        

        // // test basic put and get
        // std::cout << "put(lianap, name, Lee) = ";
        // resp = c.call("put", rowString, colString, valString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "get(lianap, name) = ";
        // resp = c.call("get", rowString, colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("Lee", std::get<1>(resp));

        // // test basic delete removes value
        // std::cout << "del(lianap, name) = ";
        // resp = c.call("del", rowString, colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "get(lianap, name) = ";
        // resp = c.call("get", rowString, colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));

        // //test delete of value not in kv store
        // std::cout << "del(random, name) = ";
        // resp = c.call("del", rowString, colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));

        // // test cput with wrong row
        // std::cout << "cput(idiot@gmail, name) = ";
        // resp = c.call("del", randomNameString, colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("No such row, column pair", std::get<1>(resp));

        // // test cput with wrong exp value
        // std::cout << "put(lianap, name, Lee) = ";
        // resp = c.call("put", rowString, colString, valString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "cput(lianap, name, wrongVal, newVal) = ";
        // resp = c.call("cput", rowString, colString, wrongValString, newValString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("Incorrect expVal", std::get<1>(resp));

        // // test cput with correct exp value
        // std::cout << "cput(lianap, name, Lee, newVal) = ";
        // resp = c.call("cput", rowString, colString, valString, newValString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));

        // std::cout << "get(lianap, name) = ";
        // resp = c.call("get", rowString, colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("newVal", std::get<1>(resp));

        // // test putting and getting nulls
        // std::cout << "put(lianap, name, LeePat) = ";
        // resp = c.call("put", rowString, colString, middleNullvalString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrect("OK", std::get<1>(resp));


        // std::cout << "get(lianap, name) = ";
        // resp = c.call("get", rowString, colString).as<resp_tuple>();
        // std::cout << std::get<1>(resp) << std::endl;
        // isCorrectStrings(middleNullvalString, std::get<1>(resp));


        


    } catch (rpc::rpc_error &e) {
        // shouldn't be reached - no exceptions currently thrown in rpc backend
        std::cout << "rpc error" << std::endl;
        
        return 1;
    }

    return 0;
}
