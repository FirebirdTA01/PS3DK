extern void draw(const int *st); extern void sky(void); extern const unsigned int *passes(void);
extern int stored[4];
int main(void) { draw(stored); sky(); return passes() != 0 ? 0 : 1; }
