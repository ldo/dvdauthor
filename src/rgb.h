static inline int RGB2Y(int r,int g,int b)
{
    return ( 257*r+504*g+ 98*b+ 16500)/1000;
}

static inline int RGB2Cr(int r,int g,int b)
{
    return ( 439*r-368*g- 71*b+128500)/1000;
}

static inline int RGB2Cb(int r,int g,int b)
{
    return (-148*r-291*g+439*b+128500)/1000;
}

static inline int RGBclamp(int x)
{
    if( x<0 ) return 0;
    if( x>255 ) return 255;
    return x;
}

static inline int YCrCb2R(int Y,int Cr,int Cb)
{
    return RGBclamp((500+1164*(Y-16)+1596*(Cr-128)              )/1000);
}

static inline int YCrCb2G(int Y,int Cr,int Cb)
{
    return RGBclamp((500+1164*(Y-16)- 813*(Cr-128)- 391*(Cb-128))/1000);
}

static inline int YCrCb2B(int Y,int Cr,int Cb)
{
    return RGBclamp((500+1164*(Y-16)              +2018*(Cb-128))/1000);
}
