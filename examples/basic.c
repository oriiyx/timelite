#include <stdio.h>

#include "timelite.h"

int main(void)
{
    /* This checks that the application can call the linked library. */
    printf("Timelite %s\n", timelite_version());
    return 0;
}
