typedef struct { unsigned int v[3]; unsigned int d[9]; } Desc;
int stored[4];
const Desc *seen[8];
void use(const Desc *d, int unit) { seen[unit] = d; }
