make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=0,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val1..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=4,0 --perf=0,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val2..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=1,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val3..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=4,0 --perf=1,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val4..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=0,0,0,0 --fq=64 --cp=8 --al=128 --lsq=64 --iq=32 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val5..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=0,0,0,0 --fq=64 --cp=32 --prf=154 --al=128 --lsq=64 --iq=32 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val6..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=4,0 --perf=1,0,0,0 --fq=64 --cp=4 --al=128 --lsq=64 --iq=32 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val7..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=0,0,0,0 --fq=64 --cp=4 --al=128 --lsq=64 --iq=32 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val8..........."
