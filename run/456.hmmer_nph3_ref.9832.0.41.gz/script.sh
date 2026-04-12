make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=0,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val9..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=4,0 --perf=0,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val10..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=1,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val11..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=4,0 --perf=1,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val12..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=4,0 --perf=1,0,0,1 -t --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val13..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=0,0,0,0 --fq=64 --cp=8 --al=128 --lsq=64 --iq=32 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val14..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=5,0 --perf=0,0,0,0 --fq=64 --cp=32 --prf=128 --al=128 --lsq=64 --iq=32 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val15..........."

make cleanrun SIM_FLAGS_EXTRA='--mdp=4,0 --perf=1,0,0,1 -t --fq=64 --cp=4 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4 -e10000000'
echo "ran val16..........."
