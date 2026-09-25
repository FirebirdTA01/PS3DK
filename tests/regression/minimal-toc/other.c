/* A second translation unit, so the program links two minimal TOCs. */
int other_counter = 40;
const char other_name[] = "minimal";

int other_add(int x)
{
    other_counter += x;
    return other_counter;
}
