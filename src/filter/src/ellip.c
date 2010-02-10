/*
 * Copyright (c) 2007, 2008, 2009, 2010 Joseph Gaeddert
 * Copyright (c) 2007, 2008, 2009, 2010 Virginia Polytechnic
 *                                      Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// Elliptic filter design
//

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "liquid.internal.h"

#define LIQUID_DEBUG_ELLIP_PRINT   0

// forward declarations

// Landen transformation (one iteration)
float landenf(float _k);

// compute elliptic integral K(k) for _n recursions
void ellipkf(float _k,
             unsigned int _n,
             float * _K,
             float * _Kp);

// elliptic degree
float ellipdegf(float _N,
                float _k1,
                unsigned int _n);

// Elliptic cd() function (_n recursions)
float cdef(float _u,
           float _k,
           unsigned int _n);

// ***************************************************************

// Landen transformation (one iteration)
float landenf(float _k)
{
    float kp = sqrtf(1.0f - _k*_k);
    return (1.0f - kp) / (1.0f + kp);
}

// compute elliptic integral K(k) for _n recursions
void ellipkf(float _k,
             unsigned int _n,
             float * _K,
             float * _Kp)
{
    float kn = _k;
    float knp = sqrtf(1.0f - _k*_k);
    float K  = 0.5f*M_PI;
    float Kp = 0.5f*M_PI;
    unsigned int i;
    for (i=0; i<_n; i++) {
        kn  = landenf(kn);
        knp = landenf(knp);

        K  *= (1.0f + kn);
        Kp *= (1.0f + knp);
    }
    *_K  = K;
    *_Kp = Kp;
}

// elliptic degree
float ellipdegf(float _N,
                float _k1,
                unsigned int _n)
{
    // compute K1, K1p from _k1
    float K1, K1p;
    ellipkf(_k1,_n,&K1,&K1p);

    // compute q1 from K1, K1p
    float q1 = expf(-M_PI*K1p/K1);

    // compute q from q1
    float q = powf(q1,1.0f/_N);

    // expand numerator, denominator
    unsigned int m;
    float b = 0.0f;
    for (m=0; m<_n; m++)
        b += powf(q,(float)(m*(m+1)));
    float a = 0.0f;
    for (m=1; m<_n; m++)
        a += powf(q,(float)(m*m));

    float g = b / (1.0f + 2.0f*a);
    float k = 4.0f*sqrtf(q)*g*g;
    return k;
}

// Elliptic cd() function (_n recursions)
float cdef(float _u,
           float _k,
           unsigned int _n)
{
    float wn = cosf(_u*M_PI*0.5f);
    float wn_inv = 1.0f / wn;
    float kn;
    unsigned int i;
    for (i=_n; i>0; i--) {
        unsigned int j;
        kn = _k;
        for (j=0; j<i; j++)
            kn = landenf(kn);

        wn = 1.0f / wn_inv;
        wn_inv = 1.0f / (1.0f + kn) * (wn_inv + kn*wn);
    }

    return 1.0f / wn_inv;
}
