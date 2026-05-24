typedef int (*kernel_fn)(int);

static volatile unsigned long kernel_sink;

static int add_one(int value)
{
    return value + 1;
}

static int scale_three(int value)
{
    return value + value + value;
}

__attribute__((used, section(".data.keep")))
kernel_fn kernel_function_table[] = {
    add_one,
    scale_three,
};

int main(void)
{
    int value = kernel_function_table[0](41);
    value = kernel_function_table[1](value);
    kernel_sink = (unsigned long)value;
    return 0;
}
