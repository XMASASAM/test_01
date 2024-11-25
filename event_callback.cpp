#include <iostream>
#include <functional>
#include <vector>
#include <set>


class MyClass2 {
   std::set<std::function<void(int)>*> call_back_listener;
public:
    void set_call_back(std::function<void(int)>* func) {
        call_back_listener.insert(func);
    }

    void raise_event() {
        for (auto call_back : call_back_listener) {
            (*call_back)(10);
        }
    }



};

class MyClass {
public:
    std::function<void(int)> call_back = std::bind(&MyClass::new_frame_func,this, std::placeholders::_1);
    void myFunction(int value,int v2) {
        std::cout << "myFunction called with value: " << value << std::endl;
        std::cout << "myFunction called with value2: " << v2 << std::endl;
    }

    void new_frame_func(int frame) {
        std::cout << "new_frame"  << frame<< std::endl;
    }




};

int main() {
    MyClass myObject;
    MyClass myObject2;
    MyClass myObject3;
    MyClass2 oo;
    oo.set_call_back(&myObject.call_back);
    oo.set_call_back(&myObject2.call_back);
    oo.set_call_back(&myObject3.call_back);
    // std::functionを使ってMyClass::myFunctionへの参照を作成
    std::function<void(int,int)> functionReference = std::bind(&MyClass::myFunction, &myObject, std::placeholders::_1,std::placeholders::_2);
    oo.raise_event();
    // functionReferenceを使ってmyFunctionを呼び出す
    functionReference(10,2); // 出力: myFunction called with value: 10
 //   System::Console::WriteLine("Stand by Ready!");
    return 0;
}
