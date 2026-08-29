!
! Copyright (C) by Argonne National Laboratory
!     See COPYRIGHT in top-level directory
!

! Copyright (c) 2001-2022, The Ohio State University. All rights
! reserved.
!
! This file is part of the MVAPICH2 software package developed by the
! team members of The Ohio State University's Network-Based Computing
! Laboratory (NBCL), headed by Professor Dhabaleswar K. (DK) Panda.
!
! For detailed copyright and licensing information, please refer to the
! copyright file COPYRIGHT in the top level MVAPICH2 directory.

#if !defined(__PGI) && !defined(__FLANG)
      subroutine real16_sum(A, B, N)
        real(16) A(N), B(N)
        integer N
        integer i

        DO i = 1, N
            B(i) = B(i) + A(i)
        END DO

        return
      end

      subroutine real16_land(A, B, N)
        real(16) A(N), B(N)
        logical C, D
        integer N
        integer i

        DO i = 1, N
            IF (B(i) /= 0) THEN
                C = .TRUE.
            ELSE
                C = .FALSE.
            ENDIF
            IF (A(i) /= 0) THEN
                D = .TRUE.
            ELSE
                D = .FALSE.
            ENDIF
            IF (C .and. D) THEN
                B(i) = 1
            ELSE
                B(i) = 0
            ENDIF
        END DO

        return
      end

      subroutine real16_lor(A, B, N)
        real(16) A(N), B(N)
        logical C, D
        integer N
        integer i

        DO i = 1, N
            IF (B(i) /= 0) THEN
                C = .TRUE.
            ELSE
                C = .FALSE.
            ENDIF
            IF (A(i) /= 0) THEN
                D = .TRUE.
            ELSE
                D = .FALSE.
            ENDIF
            IF (C .or. D) THEN
                B(i) = 1
            ELSE
                B(i) = 0
            ENDIF
        END DO

        return
      end

      subroutine real16_max(A, B, N)
        real(16) A(N), B(N)
        integer N
        integer i

        DO i = 1, N
            B(i) = MAX(B(i), A(i))
        END DO

        return
      end

      subroutine real16_maxloc(A, B, N)
        real(16) A(N), B(N)
        integer N
        integer i

        DO i = 1, N, 2
            IF ((B(i) /= B(i)) .and. (A(i) /= A(i))) THEN
                B(i+1) = MIN(B(i+1),A(i+1))
            ELSE IF ((B(i) /= B(i)) .and. (A(i) == A(i))) THEN
                B(i) = A(i)
                B(i+1) = A(i+1)
            ELSE IF ((B(i) == B(i)) .and. (A(i) /= A(i))) THEN
                cycle
            ELSE
                IF (B(i) < A(i)) THEN
                    B(i) = A(i)
                    B(i+1) = A(i+1)
                ELSE
                    B(i+1) = MIN(B(i+1),A(i+1))
                ENDIF
            ENDIF
        END DO

        return
      end

      subroutine real16_min(A, B, N)
        real(16) A(N), B(N)
        integer N
        integer i

        DO i = 1, N
            B(i) = MIN(B(i), A(i))
        END DO

        return
      end

      subroutine real16_minloc(A, B, N)
        real(16) A(N), B(N)
        integer N
        integer i

        DO i = 1, N, 2
            IF ((B(i) /= B(i)) .and. (A(i) /= A(i))) THEN
                B(i+1) = MIN(B(i+1),A(i+1))
            ELSE IF ((B(i) /= B(i)) .and. (A(i) == A(i))) THEN
                B(i) = A(i)
                B(i+1) = A(i+1)
            ELSE IF ((B(i) == B(i)) .and. (A(i) /= A(i))) THEN
                cycle
            ELSE
                IF (B(i) > A(i)) THEN
                    B(i) = A(i)
                    B(i+1) = A(i+1)
                ELSE IF (B(i) >= A(i)) THEN
                    B(i+1) = MIN(B(i+1),A(i+1))
                ENDIF
            ENDIF
        END DO

        return
      end
#elif defined(__FLANG)
! FLANG does not support real16. However, when building with CLANG compilers,
! one can use CLANG for C compiler and either FLANG or GFORTRAN for
! Fortran compilers. This is to handle both of these scenarios.
      subroutine real16_sum(A, B, N)
      end

      subroutine real16_land(A, B, N)
      end

      subroutine real16_lor(A, B, N)
      end

      subroutine real16_max(A, B, N)
      end

      subroutine real16_maxloc(A, B, N)
      end

      subroutine real16_min(A, B, N)
      end

      subroutine real16_minloc(A, B, N)
      end
#endif
