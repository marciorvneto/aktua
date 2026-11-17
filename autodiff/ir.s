// Example computation

// f(x) = (sin(x) + x^2)/tanh(x)


%0 = input       : f64[3,2]
%1 = sin  %0     : f64[3,2]
%2 = sqr  %0     : f64[3,2]
%3 = add  %1, %2 : f64[3,2]
%4 = tanh %0     : f64[3,2]
%5 = div  %3, %4 : f64[3,2]

// Reverse mode
// d(a/b) = (da * b - db*a)/b^2 = da * (1/b) + db * (-a/b^2)
// I suppose to make things easier, all operations should be done
// elementwise, including products, divisions, etc unless specified
// with, say, matmul

%6  = const 1 : f64[3,2]
%7  = inv %4 : f64[3,2]       // da term
%8  = cmul -1, %3 : f64[3,2]
%9  = sqr %4 : f64[3,2]
%10 = div %8, %9 : f64[3,2]  // db term
%11 = cos %0 : f64[3,2]
%12 = cmul 2, %0 : f64[3,2]

ret %5
