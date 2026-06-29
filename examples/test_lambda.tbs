// 1. 最基础的无参数 lambda 赋值给变量
let print_hello = () => print("Hello from zero-arg lambda!");
print_hello();

// 2. 带参数的箭头函数，用于数学计算
let square = (x) => x * x;
let add = (a, b) => a + b;

print("Square of 5 is: ", square(5));
print("5 + 10 = ", add(5, 10));

// 3. Lambda 作为高阶函数参数传递 (Callback 机制)
// 由于引擎支持将闭包/函数作为对象传递，我们可以自己写一个高阶函数

func my_map(arr, callback) {
    let result = [];
    for (var i = 0; i < arr.length(); i += 1) {
        result.push(callback(arr[i]));
    }
    return result;
}

let numbers = [1, 2, 3, 4, 5];

// 把 lambda () => {} 传进去
let doubled = my_map(numbers, (num) => num * 2);
print("Original array: ", numbers);
print("Doubled array using lambda callback: ", doubled);

// 4. 多行闭包的 Lambda
let make_counter = (start_val) => {
    var count = start_val;
    // 返回另一个闭包函数
    return () => {
        count += 1;
        return count;
    };
};

let counter = make_counter(100);
print("Counter call 1: ", counter()); // 101
print("Counter call 2: ", counter()); // 102

// 5. 立即执行的匿名 lambda (Immediately Invoked Function Expression)
let temp_result = ((base, exponent) => {
    var res = 1;
    for (var i = 0; i < exponent; i += 1) {
        res *= base;
    }
    return res;
})(2, 10);

print("2^10 calculated inline by IIFE lambda: ", temp_result);
