#pragma once

#include "protocol.hpp"

namespace sense {
    
class SenseInterface {
    public:

        SenseInterface() {};
        
        struct State;

        void read_state(State *out);                

        void init();
        void task();
        void deinit();

    private:
        void write_state();


};


class SenseRebounder : SenseInterface {

};

class SenseButton : SenseInterface {

}

}