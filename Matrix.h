#pragma once
#include <array>
#include <cmath>
#include <stdexcept>

//small 2x2 matrix helpers, inline as they are short
using Vec2 = std::array<double, 2>;
using Mat2 = std::array<std::array<double, 2>, 2>;

inline Mat2 multiply(const Mat2& matA, const Mat2& matB) {
    Mat2 matC{}; //initilise -> as 0
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < 2; j++) {
            for (size_t k = 0; k < 2; k++) {
                matC[i][j] += matA[i][k] * matB[k][j];
            }
        }
    }
    return matC;
}

inline Mat2 transpose(const Mat2& matA) {
    return {{{matA[0][0], matA[1][0]},
             {matA[0][1], matA[1][1]}}};
}

inline double determinant(const Mat2& matA) {
    return matA[0][0] * matA[1][1] - matA[0][1] * matA[1][0];
}

inline Mat2 inverse(const Mat2& matA) {
    double detA = determinant(matA);
    //near zero det -> no inverse, fail loudly rather than blow up the filter
    if (std::abs(detA) < 1e-12) throw std::runtime_error("tried inverse singular matrix");

    //adjugate over det -> swap diagonal, negate off diagonal
    Mat2 inverseMat;
    inverseMat[0][0] = matA[1][1] / detA;
    inverseMat[0][1] = -matA[0][1] / detA;
    inverseMat[1][0] = -matA[1][0] / detA;
    inverseMat[1][1] = matA[0][0] / detA;
    return inverseMat;
}

// solves ev^2 - trace.ev + det = 0 (quad formula) -> returns {larger, smaller}
inline Vec2 eigenvalues(const Mat2& matA) {
    double detA = determinant(matA);
    double trace = matA[0][0] + matA[1][1];
    double discriminant = trace*trace - 4.0*detA; //b^2 -  4ac
    if (discriminant < 0) discriminant = 0; //clamp small negtive values (for sqrt)

    double root = std::sqrt(discriminant);
    return {(trace + root) / 2.0, (trace - root) / 2.0};
}