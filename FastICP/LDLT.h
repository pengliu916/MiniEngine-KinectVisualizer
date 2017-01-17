
class LDLT
{
private:
    float a[36];
    bool fullRank;

public:
    LDLT (const float *mat) {
        for (int i = 0; i < 36; i++) {
            a[i] = mat[i];
        }

        for (int c = 0; c < 6; c++) {
            float inv_diag = 1;
            for (int r = c; r < 6; r++) {
                float val = a[c + r * 6];
                for (int c2 = 0; c2 < c; c2++) {
                    val -= a[c + c2 * 6] * a[c2 + r * 6];
                }
                if (r == c) {
                    if (val == 0) {
                        fullRank = false;
                        return;
                    }
                    a[c + r * 6] = val;
                    inv_diag = 1.0f / val;
                } else {
                    a[r + c * 6] = val;
                    a[c + r * 6] = val * inv_diag;
                }
            }
        }
        fullRank = true;
    }

    bool Backsub(float *result, const float *v) const
    {
        if (!fullRank) {
            return false;
        }
        float y[6];
        for (int i = 0; i < 6; i++) {
            float val = v[i];
            for (int j = 0; j < i; j++) {
                val -= a[j + i * 6] * y[j];
            }
            y[i] = val;
        }

        for (int i = 0; i < 6; i++) {
            y[i] /= a[i + i * 6];
        }

        for (int i = 5; i >= 0; i--) {
            float val = y[i];
            for (int j = i + 1; j < 6; j++) {
                val -= a[i + j * 6] * result[j];
            }
            result[i] = val;
        }
        return true;
    }
};